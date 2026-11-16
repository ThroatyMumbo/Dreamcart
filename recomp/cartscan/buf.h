#ifndef CARTSCAN_BUF_H
#define CARTSCAN_BUF_H

#include <string.h>

#include "scan.h"

// A growing byte buffer of little-endian words, for meta.c and metacache.c
typedef struct { uint8_t *b; uint32_t n, cap; } Buf;

static inline void put(Buf *o, const void *p, uint32_t n) {
    if (o->n + n > o->cap) {
        while (o->n + n > o->cap) o->cap = o->cap ? 2 * o->cap : 65536;
        o->b = cs_realloc(o->b, o->cap);
    }
    if (p) memcpy(o->b + o->n, p, n);
    o->n += n;
}

static inline void u32(Buf *o, uint32_t v) {
    uint8_t le[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(o, le, 4);
}

#endif
