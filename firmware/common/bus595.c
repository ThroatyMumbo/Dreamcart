#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pads.h"
#include "bus595.h"

static bus595_t g_bus;

static uint32_t cycles_less(uint32_t ns, uint32_t fixed) {
    uint32_t c = (uint32_t)(((uint64_t)ns * g_bus.hz + 999999999u) / 1000000000u);
    return c > fixed ? c - fixed : 0;
}

// SM disabled: OSR <- v, then dst <- OSR
static void load_reg(uint sm, uint32_t v, enum pio_src_dest dst) {
    pio_sm_put(g_bus.pio, sm, v);
    pio_sm_exec(g_bus.pio, sm, pio_encode_pull(false, true));
    pio_sm_exec(g_bus.pio, sm, pio_encode_mov(dst, pio_osr));
}

void bus595_set_hz(uint32_t hz, uint32_t access_ns, uint32_t wr_ns) {
    PIO pio = g_bus.pio;
    g_bus.hz = hz;
    float div = (float)clock_get_hz(clk_sys) / (float)hz;
    pio_sm_set_enabled(pio, g_bus.rd_sm, false);
    pio_sm_set_enabled(pio, g_bus.wr_sm, false);
    pio_sm_set_clkdiv(pio, g_bus.rd_sm, div);
    pio_sm_set_clkdiv(pio, g_bus.wr_sm, div);
    load_reg(g_bus.rd_sm, cycles_less(access_ns, 3), pio_y);
    load_reg(g_bus.wr_sm, cycles_less(wr_ns, 3), pio_isr);
    pio_sm_set_enabled(pio, g_bus.rd_sm, true);
    pio_sm_set_enabled(pio, g_bus.wr_sm, true);
}

static void wait_idle(uint sm, uint off) {
    while (!pio_sm_is_tx_fifo_empty(g_bus.pio, sm) || pio_sm_get_pc(g_bus.pio, sm) != off) tight_loop_contents();
}

static void feed(const uint32_t *tab, uint sm, uint32_t n) {
    dma_channel_config c = dma_channel_get_default_config(g_bus.tx_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(g_bus.pio, sm, true));
    dma_channel_configure(g_bus.tx_dma, &c, &g_bus.pio->txf[sm], tab, n, true);
}

void bus595_read(const uint32_t *tab, void *dst, uint32_t n, enum dma_channel_transfer_size size, bool bswap) {
    dma_channel_config c = dma_channel_get_default_config(g_bus.rx_dma);
    channel_config_set_transfer_data_size(&c, size);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_bswap(&c, bswap);
    channel_config_set_dreq(&c, pio_get_dreq(g_bus.pio, g_bus.rd_sm, false));
    dma_channel_configure(g_bus.rx_dma, &c, dst, &g_bus.pio->rxf[g_bus.rd_sm], n, true);
    feed(tab, g_bus.rd_sm, n);
    dma_channel_wait_for_finish_blocking(g_bus.rx_dma);
    wait_idle(g_bus.rd_sm, g_bus.rd_off);
}

void bus595_write(const uint32_t *tab, uint32_t n) {
    feed(tab, g_bus.wr_sm, n);
    dma_channel_wait_for_finish_blocking(g_bus.tx_dma);
    wait_idle(g_bus.wr_sm, g_bus.wr_off);
}

// The ctl pins were already at idle on SIO, so handing them to the PIO doesn't glitch.
const bus595_t *bus595_init(PIO pio, const pio_program_t *rd, const pio_program_t *wr,
                            uint32_t ctl, uint32_t data, uint32_t idle) {
    g_bus.pio = pio;
    for (uint p = 0; p < 32; p++) if ((ctl | data) >> p & 1) pad_fast(p);
    g_bus.rd_off = pio_add_program(pio, rd);
    g_bus.wr_off = pio_add_program(pio, wr);
    g_bus.rd_sm = (uint)pio_claim_unused_sm(pio, true);
    g_bus.wr_sm = (uint)pio_claim_unused_sm(pio, true);
    pio_sm_set_pins_with_mask(pio, g_bus.rd_sm, idle, ctl);
    pio_sm_set_pindirs_with_mask(pio, g_bus.rd_sm, ctl, ctl | data);
    for (uint p = 0; p < 32; p++) if ((ctl | data) >> p & 1) pio_gpio_init(pio, p);
    g_bus.tx_dma = (uint)dma_claim_unused_channel(true);
    g_bus.rx_dma = (uint)dma_claim_unused_channel(true);
    return &g_bus;
}
