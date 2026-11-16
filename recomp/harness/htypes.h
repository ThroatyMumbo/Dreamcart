#ifndef HTYPES_H
#define HTYPES_H

#if __has_include(<stdint.h>)
#include <stdint.h>
#else
typedef __UINT32_TYPE__ uint32_t;
typedef __INT32_TYPE__ int32_t;
typedef __UINT16_TYPE__ uint16_t;
typedef __UINT8_TYPE__ uint8_t;
typedef __INT16_TYPE__ int16_t;
typedef __INT8_TYPE__ int8_t;
typedef __UINT64_TYPE__ uint64_t;
#endif

static inline int32_t sat_i16(int32_t x) {  // one compare
    if ((uint32_t)x + 0x8000u > 0xFFFFu) x = (x >> 31) ^ 0x7FFF;
    return x;
}

#ifndef RDRAM_BYTES
#define RDRAM_BYTES 0x400000u  // make RDRAM_MB=8: an Expansion Pak
#endif

#endif
