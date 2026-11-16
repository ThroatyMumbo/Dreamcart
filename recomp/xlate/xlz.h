#ifndef XLZ_H
#define XLZ_H

// LZ4's block format (lz4.org), one independent block of at most 64 KB per call
#include <stdint.h>

#define XLZ_WORK 4096u  // the compressor's hash table, in uint16_t

uint32_t xlz_bound(uint32_t n);  // the most xlz_compress writes for n bytes
uint32_t xlz_compress(const uint8_t *src, uint32_t n, uint8_t *dst, uint16_t *work);
// The bytes written to dst, or -1 when src isn't one well-formed block that fits cap
int32_t xlz_decompress(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap);

#endif
