#include "crc32.h"

static uint32_t table[256];

uint32_t crc32_update(uint32_t crc, const void *p, uint32_t n) {
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    const uint8_t *b = p;
    crc = ~crc;
    while (n--) crc = table[(crc ^ *b++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
