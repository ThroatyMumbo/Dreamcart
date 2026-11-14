#include "hardware/dma.h"
#include "crc_dma.h"

static uint g_crc_dma;
static dma_channel_config g_crc_cfg;

// byte transfers so the sniffer sees the payload in wire order
uint32_t crc_dma(const uint8_t *p, uint32_t len) {
    static uint32_t sink;
    dma_sniffer_set_data_accumulator(0xFFFFFFFFu);
    dma_channel_configure(g_crc_dma, &g_crc_cfg, &sink, p, len, true);
    dma_channel_wait_for_finish_blocking(g_crc_dma);
    return dma_sniffer_get_data_accumulator();
}

void crc_dma_init(void) {
    g_crc_dma = (uint)dma_claim_unused_channel(true);
    g_crc_cfg = dma_channel_get_default_config(g_crc_dma);
    channel_config_set_transfer_data_size(&g_crc_cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&g_crc_cfg, true);
    channel_config_set_write_increment(&g_crc_cfg, false);
    channel_config_set_dreq(&g_crc_cfg, DREQ_FORCE);
    channel_config_set_sniff_enable(&g_crc_cfg, true);
    dma_sniffer_enable(g_crc_dma, 0x1, true);     // bit-reversed data, reversed and inverted out: zlib
    dma_sniffer_set_output_reverse_enabled(true);
    dma_sniffer_set_output_invert_enabled(true);
}
