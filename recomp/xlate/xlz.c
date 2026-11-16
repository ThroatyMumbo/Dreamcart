#include "xlz.h"

#include <string.h>

#define MINMATCH 4
#define MFLIMIT  12  // no match starts in a block's last 12 bytes
#define LASTLIT  5   // and its last 5 are always literals
#define HBITS    12

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint32_t hash4(uint32_t v) { return (v * 2654435761u) >> (32 - HBITS); }

static uint8_t *put_len(uint8_t *o, uint32_t n) {
    for (; n >= 255; n -= 255) *o++ = 255;
    *o++ = (uint8_t)n;
    return o;
}

static uint8_t *put_seq(uint8_t *op, const uint8_t *lit, uint32_t nlit) {
    *op++ = (uint8_t)((nlit >= 15 ? 15 : nlit) << 4);
    if (nlit >= 15) op = put_len(op, nlit - 15);
    memcpy(op, lit, nlit);
    return op + nlit;
}

uint32_t xlz_bound(uint32_t n) { return n + n / 255 + 16; }

uint32_t xlz_compress(const uint8_t *src, uint32_t n, uint8_t *dst, uint16_t *work) {
    const uint8_t *ip = src, *anchor = src, *end = src + n;
    uint8_t *op = dst;
    if (n > MFLIMIT) {
        const uint8_t *mlimit = end - MFLIMIT, *mend = end - LASTLIT;
        memset(work, 0, XLZ_WORK * sizeof *work);
        for (ip++; ip < mlimit;) {
            uint32_t v = rd32(ip), h = hash4(v);
            const uint8_t *ref = src + work[h];
            work[h] = (uint16_t)(ip - src);
            if (ref >= ip || rd32(ref) != v) {
                ip++;
                continue;
            }
            while (ip > anchor && ref > src && ip[-1] == ref[-1]) ip--, ref--;
            const uint8_t *m = ip + MINMATCH, *r = ref + MINMATCH;
            while (m < mend && *m == *r) m++, r++;
            uint32_t nlit = (uint32_t)(ip - anchor), ml = (uint32_t)(m - ip) - MINMATCH, off = (uint32_t)(ip - ref);
            uint8_t *tok = op;
            op = put_seq(op, anchor, nlit);
            *op++ = (uint8_t)off, *op++ = (uint8_t)(off >> 8);
            *tok |= (uint8_t)(ml >= 15 ? 15 : ml);
            if (ml >= 15) op = put_len(op, ml - 15);
            ip = anchor = m;
        }
    }
    return (uint32_t)(put_seq(op, anchor, (uint32_t)(end - anchor)) - dst);
}

int32_t xlz_decompress(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap) {
    const uint8_t *ip = src, *iend = src + n;
    uint8_t *op = dst, *oend = dst + cap;
    while (ip < iend) {
        uint32_t tok = *ip++, nlit = tok >> 4, ml = tok & 15;
        if (nlit == 15)
            for (uint8_t b = 255; b == 255; nlit += b) {
                if (ip >= iend) return -1;
                b = *ip++;
            }
        if (nlit > (uint32_t)(iend - ip) || nlit > (uint32_t)(oend - op)) return -1;
        memcpy(op, ip, nlit);
        op += nlit, ip += nlit;
        if (ip == iend) return (int32_t)(op - dst);
        if (iend - ip < 2) return -1;
        uint32_t off = ip[0] | (uint32_t)ip[1] << 8;
        ip += 2;
        if (!off || off > (uint32_t)(op - dst)) return -1;
        if (ml == 15)
            for (uint8_t b = 255; b == 255; ml += b) {
                if (ip >= iend) return -1;
                b = *ip++;
            }
        ml += MINMATCH;
        if (ml > (uint32_t)(oend - op)) return -1;
        const uint8_t *r = op - off;
        if (off >= ml) {
            memcpy(op, r, ml);
            op += ml;
        } else {
            while (ml--) *op++ = *r++;
        }
    }
    return -1;
}
