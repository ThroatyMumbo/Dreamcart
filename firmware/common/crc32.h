// zlib CRC-32 by table, for core0; core1's Maple replies use crc_dma instead.
#pragma once
#include <stdint.h>

uint32_t crc32_update(uint32_t crc, const void *p, uint32_t n);   // crc 0 to start
