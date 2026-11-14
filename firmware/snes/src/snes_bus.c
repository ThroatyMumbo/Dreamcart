#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "board.h"
#include "fault.h"
#include "pads.h"
#include "bus595.h"
#include "snes_map.h"
#include "snes_bus.h"
#include "snes_bus.pio.h"

_Static_assert(PIN_WR == PIN_RD + 1, "snes_bus.pio sets /RD and /WR as one group");
_Static_assert(PIN_ADDR_SRCLK == PIN_ADDR_SER + 1 && PIN_ADDR_RCLK == PIN_ADDR_SER + 2,
               "snes_bus.pio side-sets SER, SRCLK and RCLK as one group");

#define ACCESS_NS 400u      // RCLK to sample; OSCR waits 375 ns, against 200 ns SlowROM
#define WR_NS     300u      // /WR low; OSCR holds 1 us, SRAM needs well under 100 ns
#define BURST     512u

static const bus595_t *g_bus;
static uint32_t g_tab[BURST];

bool snes_bus_set_hz(uint32_t hz) {
    if (hz < SNES_BUS_HZ_MIN || hz > SNES_BUS_HZ_MAX) return false;
    bus595_set_hz(hz, ACCESS_NS, WR_NS);
    return true;
}

uint32_t snes_bus_hz(void) { return g_bus->hz; }

// Inside one 32 KB half-bank /ROMSEL and the bank byte hold, so the table is linear.
static uint32_t chunk(uint32_t bus, uint32_t n) {
    uint32_t k = 0x8000u - (bus & 0x7FFFu);
    if (k > BURST) k = BURST;
    return k < n ? k : n;
}

static uint32_t next(uint32_t bus, uint32_t k) { return (bus & 0xFF0000u) | ((bus + k) & 0xFFFFu); }

static void romsel(uint32_t bus) { gpio_put(PIN_ROMSEL, !snes_romsel_low(bus)); }

void snes_bus_read(uint32_t bus, uint8_t *dst, uint32_t n) {
    while (n) {
        uint32_t k = chunk(bus, n);
        for (uint32_t i = 0; i < k; i++) g_tab[i] = (bus + i) << 8;
        romsel(bus);
        bus595_read(g_tab, dst, k, DMA_SIZE_8, false);
        bus = next(bus, k);
        dst += k;
        n -= k;
    }
}

// D0-7 are outputs only while a write burst runs; /RD stays high throughout, so nothing on the cart drives them.
void snes_bus_write(uint32_t bus, const uint8_t *src, uint32_t n) {
    pio_sm_exec(g_bus->pio, g_bus->wr_sm, pio_encode_mov_not(pio_pindirs, pio_null));
    while (n) {
        uint32_t k = chunk(bus, n);
        for (uint32_t i = 0; i < k; i++) g_tab[i] = (bus + i) << 8 | src[i];
        romsel(bus);
        bus595_write(g_tab, k);
        bus = next(bus, k);
        src += k;
        n -= k;
    }
    pio_sm_exec(g_bus->pio, g_bus->wr_sm, pio_encode_mov(pio_pindirs, pio_null));
}

void snes_bus_init(void) {
    MARK(5);
    const uint32_t ctl = 3u << PIN_RD | 7u << PIN_ADDR_SER;
    const uint32_t data = 0xFFu << PIN_D0;
    pad_fast(PIN_ROMSEL);
    g_bus = bus595_init(PIO_CART_BUS, &snes_read_program, &snes_write_program, ctl, data, 3u << PIN_RD);
    snes_read_program_init(g_bus->pio, g_bus->rd_sm, g_bus->rd_off, PIN_D0, PIN_RD, PIN_ADDR_SER);
    snes_write_program_init(g_bus->pio, g_bus->wr_sm, g_bus->wr_off, PIN_D0, PIN_RD, PIN_ADDR_SER);
    snes_bus_set_hz(SNES_BUS_HZ_DEFAULT);
    MARK(6);
}
