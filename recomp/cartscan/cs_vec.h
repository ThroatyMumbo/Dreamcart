#ifndef CARTSCAN_CS_VEC_H
#define CARTSCAN_CS_VEC_H

#include <stdint.h>
#include <string.h>

#include "scan.h"

// scan.c's containers: sorted u32 vectors, an open-addressed u32 map, and per-page address bitsets
typedef int64_t i64;
typedef uint64_t u64;
#define NONE (-1LL)

typedef struct { uint32_t *v; uint32_t n, cap; } Vec;

static void vpush(Vec *a, uint32_t x) {
    if (a->n == a->cap) {
        a->cap = a->cap ? 2 * a->cap : 8;
        a->v = cs_realloc(a->v, a->cap * 4);
    }
    a->v[a->n++] = x;
}

static void vfree(Vec *a) {
    cs_release(a->v);
    memset(a, 0, sizeof *a);
}

// LSD radix sort, 8 bits a pass; qsort's calls per compare dominate on the SH-4
static void radix_u32(uint32_t *v, uint32_t n) {
    uint32_t *tmp = cs_alloc(n * 4), *src = v, *dst = tmp;
    for (int sh = 0; sh < 32; sh += 8) {
        uint32_t cnt[257] = {0};
        for (uint32_t i = 0; i < n; i++) cnt[((src[i] >> sh) & 0xFF) + 1]++;
        for (int b = 0; b < 256; b++) cnt[b + 1] += cnt[b];
        for (uint32_t i = 0; i < n; i++) dst[cnt[(src[i] >> sh) & 0xFF]++] = src[i];
        uint32_t *t = src;
        src = dst, dst = t;
    }
    cs_release(tmp);
}

static void vsortu(Vec *a) {
    if (a->n < 2) return;
    if (a->n <= 32) {
        for (uint32_t i = 1; i < a->n; i++) {
            uint32_t x = a->v[i], j = i;
            while (j > 0 && a->v[j - 1] > x) a->v[j] = a->v[j - 1], j--;
            a->v[j] = x;
        }
    } else {
        radix_u32(a->v, a->n);
    }
    uint32_t k = 1;
    for (uint32_t i = 1; i < a->n; i++)
        if (a->v[i] != a->v[k - 1]) a->v[k++] = a->v[i];
    a->n = k;
}

static int veq(const Vec *a, const Vec *b) { return a->n == b->n && !memcmp(a->v, b->v, a->n * 4); }

// bisect_right(v, x) - 1
static i64 floor_idx(const uint32_t *v, uint32_t n, uint32_t x) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (v[m] <= x) lo = m + 1;
        else hi = m;
    }
    return (i64)lo - 1;
}

typedef struct { uint32_t *key, *val; uint8_t *st; uint32_t cap, n, used; } Map;  // st: 0 empty, 1 full, 2 deleted

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

static i64 map_slot(const Map *m, uint32_t k) {
    if (!m->cap) return -1;
    for (uint32_t i = hash32(k) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (m->st[i] == 0) return -1;
        if (m->st[i] == 1 && m->key[i] == k) return i;
    }
}

static void map_put(Map *m, uint32_t k, uint32_t v);

static void map_grow(Map *m) {
    Map o = *m;
    uint32_t cap = 16;
    while (cap < 4 * (o.n + 1)) cap *= 2;
    m->cap = cap, m->n = m->used = 0;
    m->key = cs_alloc(cap * 4), m->val = cs_alloc(cap * 4), m->st = cs_alloc(cap);
    memset(m->st, 0, cap);
    for (uint32_t i = 0; i < o.cap; i++)
        if (o.st[i] == 1) map_put(m, o.key[i], o.val[i]);
    cs_release(o.key), cs_release(o.val), cs_release(o.st);
}

static void map_put(Map *m, uint32_t k, uint32_t v) {
    i64 s = map_slot(m, k);
    if (s >= 0) {
        m->val[s] = v;
        return;
    }
    if (4 * (m->used + 1) > 3 * m->cap) map_grow(m);
    uint32_t i = hash32(k) & (m->cap - 1);
    while (m->st[i] == 1) i = (i + 1) & (m->cap - 1);
    if (m->st[i] == 0) m->used++;
    m->st[i] = 1, m->key[i] = k, m->val[i] = v, m->n++;
}

static int map_has(const Map *m, uint32_t k) { return map_slot(m, k) >= 0; }
static void map_add(Map *m, uint32_t k) { map_put(m, k, 0); }

static i64 map_get(const Map *m, uint32_t k) {
    i64 s = map_slot(m, k);
    return s < 0 ? NONE : m->val[s];
}

static int map_del(Map *m, uint32_t k) {
    i64 s = map_slot(m, k);
    if (s < 0) return 0;
    m->st[s] = 2, m->n--;
    return 1;
}

static void map_clear(Map *m) {
    if (m->cap) memset(m->st, 0, m->cap);
    m->n = m->used = 0;
}

static void map_free(Map *m) {
    cs_release(m->key), cs_release(m->val), cs_release(m->st);
    memset(m, 0, sizeof *m);
}

// Address sets that the scan tests on every instruction: a bitmap per 4 KB page, with odd addresses on the side
typedef struct {
    Map pages, odd;
    uint32_t (*bits)[32];
    uint32_t npages, cap, lastkey;
    i64 lastidx;
} Bits;

static i64 bits_page(Bits *b, uint32_t key, int create) {
    if (b->lastidx != NONE && key == b->lastkey) return b->lastidx;
    i64 i = map_get(&b->pages, key);
    if (i == NONE && create) {
        if (b->npages == b->cap) {
            b->cap = b->cap ? 2 * b->cap : 64;
            b->bits = cs_realloc(b->bits, b->cap * sizeof *b->bits);
        }
        memset(b->bits[b->npages], 0, sizeof *b->bits);
        i = b->npages++;
        map_put(&b->pages, key, (uint32_t)i);
    }
    if (i != NONE) b->lastkey = key, b->lastidx = i;
    return i;
}

static int bits_has(Bits *b, uint32_t a) {
    if (a & 3) return map_has(&b->odd, a);
    i64 i = bits_page(b, a >> 12, 0);
    return i != NONE && (b->bits[i][(a >> 7) & 31] >> ((a >> 2) & 31) & 1);
}

// Both return whether the set changed
static int bits_add(Bits *b, uint32_t a) {
    if (a & 3) {
        int had = map_has(&b->odd, a);
        map_add(&b->odd, a);
        return !had;
    }
    i64 i = bits_page(b, a >> 12, 1);
    uint32_t *w = &b->bits[i][(a >> 7) & 31], m = 1u << ((a >> 2) & 31);
    if (*w & m) return 0;
    *w |= m;
    return 1;
}

static int bits_del(Bits *b, uint32_t a) {
    if (a & 3) return map_del(&b->odd, a);
    i64 i = bits_page(b, a >> 12, 0);
    if (i == NONE) return 0;
    uint32_t *w = &b->bits[i][(a >> 7) & 31], m = 1u << ((a >> 2) & 31);
    if (!(*w & m)) return 0;
    *w &= ~m;
    return 1;
}

static void bits_clear(Bits *b) {
    if (b->npages) memset(b->bits, 0, b->npages * sizeof *b->bits);
    map_clear(&b->odd);
}

static void bits_free(Bits *b) {
    map_free(&b->pages), map_free(&b->odd);
    cs_release(b->bits);
    memset(b, 0, sizeof *b);
    b->lastidx = NONE;
}

#endif
