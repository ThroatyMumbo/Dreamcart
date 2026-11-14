#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "board.h"
#include "fault.h"
#include "pads.h"
#include "bus595.h"
#include "md_bus.h"
#include "md_bus.pio.h"

_Static_assert(PIN_OE == PIN_CE + 1 && PIN_AS == PIN_CE + 2 && PIN_ASEL == PIN_CE + 3,
               "md_bus.pio sets /CE /OE /AS /ASEL as one group");
_Static_assert(PIN_ADDR_SRCLK == PIN_ADDR_SER + 1 && PIN_ADDR_RCLK == PIN_ADDR_SER + 2,
               "md_bus.pio side-sets SER, SRCLK and RCLK as one group");
_Static_assert(PIN_VD0 == 0 && PIN_UWR < 32, "md_write's image spans GP0 to /UWR");

#define ACCESS_NS 400u      // RCLK to sample; OSCR waits 375 ns, against 200 ns mask ROMs
#define WR_NS     400u      // strobe low; OSCR holds at least 200 ns
#define BURST     256u
#define DATA_MASK 0xFFFFu
#define IDLE      (0xFu << PIN_CE | 1u << PIN_LWR | 1u << PIN_UWR)

static const bus595_t *g_bus;
static uint32_t g_tab[3 * BURST];

bool md_bus_set_hz(uint32_t hz) {
    if (hz < MD_BUS_HZ_MIN || hz > MD_BUS_HZ_MAX) return false;
    bus595_set_hz(hz, ACCESS_NS, WR_NS);
    return true;
}

uint32_t md_bus_hz(void) { return g_bus->hz; }

static uint32_t addr_word(uint32_t word) { return (word & 0x7FFFFFu) << 8; }

void md_bus_read(uint32_t word, uint8_t *dst, uint32_t nwords) {
    while (nwords) {
        uint32_t k = nwords < BURST ? nwords : BURST;
        for (uint32_t i = 0; i < k; i++) g_tab[i] = addr_word(word + i);
        bus595_read(g_tab, dst, k, DMA_SIZE_16, true);     // byte-swapped: VD15-8 is the first byte of the word in a .bin
        word += k;
        dst += 2 * k;
        nwords -= k;
    }
}

// VD0-15 are outputs only around a write; the SM is stopped so the forced SETs can't race it.
static void data_out(bool out) {
    pio_sm_set_enabled(g_bus->pio, g_bus->wr_sm, false);
    pio_sm_set_pindirs_with_mask(g_bus->pio, g_bus->wr_sm, out ? DATA_MASK : 0, DATA_MASK);
    pio_sm_set_enabled(g_bus->pio, g_bus->wr_sm, true);
}

static uint32_t strobe_bits(uint32_t strobes) {
    return (strobes & MD_STROBE_CE ? 1u << PIN_CE : 0) | (strobes & MD_STROBE_LWR ? 1u << PIN_LWR : 0) |
           (strobes & MD_STROBE_UWR ? 1u << PIN_UWR : 0);
}

static void write_words(uint32_t word, const uint16_t *src, uint32_t nwords, uint32_t strobes) {
    uint32_t low = strobe_bits(strobes);
    while (nwords) {
        uint32_t k = nwords < BURST ? nwords : BURST;
        for (uint32_t i = 0; i < k; i++) {
            uint32_t idle = IDLE | src[i];
            g_tab[3 * i] = addr_word(word + i);
            g_tab[3 * i + 1] = idle & ~low;
            g_tab[3 * i + 2] = idle;
        }
        bus595_write(g_tab, 3 * k);
        word += k;
        src += k;
        nwords -= k;
    }
}

// /OE stays high throughout, so nothing on the cart drives VD0-15 while they are outputs.
void md_bus_write(uint32_t word, const uint16_t *src, uint32_t nwords, uint32_t strobes) {
    data_out(true);
    write_words(word, src, nwords, strobes);
    data_out(false);
}

// The data is still driven when /TIME rises, for carts that latch on that edge.
void md_bus_reg_write(uint32_t word, uint16_t val) {
    data_out(true);
    gpio_put(PIN_TIME, 0);
    write_words(word, &val, 1, MD_STROBE_LWR);
    gpio_put(PIN_TIME, 1);
    data_out(false);
}

void md_bus_init(void) {
    MARK(5);
    const uint32_t ctl = 0xFu << PIN_CE | 7u << PIN_ADDR_SER | 1u << PIN_LWR | 1u << PIN_UWR;
    const uint32_t data = DATA_MASK << PIN_VD0;
    pad_fast(PIN_TIME);
    g_bus = bus595_init(PIO_CART_BUS, &md_read_program, &md_write_program, ctl, data, IDLE);    // GP23 stays on GPOUT1
    md_read_program_init(g_bus->pio, g_bus->rd_sm, g_bus->rd_off, PIN_VD0, PIN_CE, PIN_ADDR_SER);
    md_write_program_init(g_bus->pio, g_bus->wr_sm, g_bus->wr_off, PIN_VD0, PIN_UWR + 1, PIN_ADDR_SER);
    md_bus_set_hz(MD_BUS_HZ_DEFAULT);
    MARK(6);
}
