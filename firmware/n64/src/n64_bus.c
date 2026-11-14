#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include "board.h"
#include "fault.h"
#include "pads.h"
#include "n64_bus.h"
#include "n64_bus.pio.h"

_Static_assert(PIN_ALE_H == PIN_ALE_L + 1 && PIN_RD == PIN_ALE_L + 2 && PIN_WR == PIN_ALE_L + 3,
               "n64_bus.pio side-sets ALE_L, ALE_H and /RD and sets /WR as one consecutive group");

static PIO  bus_pio = PIO_CART_BUS;
static uint bus_sm, bus_offset, bus_dma;
static bool g_write_cpu;

static void read_dma(uint32_t addr, void *dst, uint32_t n, bool big_endian) {
    dma_channel_config c = dma_channel_get_default_config(bus_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_bswap(&c, big_endian);
    channel_config_set_dreq(&c, pio_get_dreq(bus_pio, bus_sm, false));
    dma_channel_configure(bus_dma, &c, dst, &bus_pio->rxf[bus_sm], n / 2, true);
    pio_sm_put_blocking(bus_pio, bus_sm, n / 2 - 1);
    pio_sm_put_blocking(bus_pio, bus_sm, addr);
    dma_channel_wait_for_finish_blocking(bus_dma);
}

void n64_bus_read(uint32_t addr, void *dst, uint32_t n) { read_dma(addr, dst, n, true); }

void n64_bus_read_words(uint32_t addr, uint16_t *out, uint32_t words) { read_dma(addr, out, words * 2, false); }

static void wait_sm_idle(void) {
    while (!pio_sm_is_tx_fifo_empty(bus_pio, bus_sm) || pio_sm_get_pc(bus_pio, bus_sm) != bus_offset)
        tight_loop_contents();
}

// DMA feeds the burst so an IRQ can't stall /WR mid-block; the EverDrive drops the rest of a stalled burst.
void n64_bus_write_words(uint32_t addr, const uint16_t *words, uint32_t count) {
    pio_sm_put_blocking(bus_pio, bus_sm, 0x80000000u | (count - 1));
    pio_sm_put_blocking(bus_pio, bus_sm, addr);
    if (g_write_cpu) {
        for (uint32_t i = 0; i < count; i++) pio_sm_put_blocking(bus_pio, bus_sm, words[i]);
    } else {
        dma_channel_config c = dma_channel_get_default_config(bus_dma);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(bus_pio, bus_sm, true));
        dma_channel_configure(bus_dma, &c, &bus_pio->txf[bus_sm], words, count, true);
        dma_channel_wait_for_finish_blocking(bus_dma);
    }
    wait_sm_idle();
}

bool n64_bus_toggle_write_cpu(void) { return g_write_cpu = !g_write_cpu; }

void n64_bus_init(void) {
    MARK(4);
    for (uint p = AD_BASE; p < AD_BASE + 16; p++) pad_fast(p);
    for (uint p = PIN_ALE_L; p <= PIN_WR; p++) pad_fast(p);
    pad_fast(PIN_RESET);
    gpio_init(PIN_RESET);
    gpio_put(PIN_RESET, 0);
    gpio_set_dir(PIN_RESET, GPIO_OUT);
    MARK(5);

    // the SM starts with /RD high and AD released, so the ROM's outputs are deasserted before AD is ever driven
    bus_offset = pio_add_program(bus_pio, &n64_bus_program);
    bus_sm = pio_claim_unused_sm(bus_pio, true);
    n64_bus_program_init(bus_pio, bus_sm, bus_offset, AD_BASE, PIN_ALE_L);
    bus_dma = dma_claim_unused_channel(true);
    MARK(6);
}

void n64_bus_set_reset(bool released) { gpio_put(PIN_RESET, released); }
