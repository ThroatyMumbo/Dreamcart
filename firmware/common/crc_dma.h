#pragma once
#include <stdint.h>

// Claims a DMA channel and the sniffer; one owner of the sniffer per build.
void crc_dma_init(void);

// zlib's CRC-32 of p, from the DMA sniffer; blocking, a single core at a time.
uint32_t crc_dma(const uint8_t *p, uint32_t len);
