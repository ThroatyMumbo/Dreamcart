#include "pi_ledger.h"

// Recent PI DMAs by destination, small and bank-sized apart; searched once a note
#define SMALL_SPANS 4096
#define BIG_SPANS 64
struct pi_span { uint32_t dram, len, rom, seq; };
static struct pi_span spans_small[SMALL_SPANS], spans_big[BIG_SPANS];
static uint32_t n_small, n_big, pi_seq;

// Recent answers: [lo, hi) maps to ROM by delta, no newer DMA over it; a DMA landing on one drops it
static struct { uint32_t lo, hi, delta; } hits[4];
static uint32_t next_hit;

void pi_ledger_add(uint32_t rom, uint32_t dram, uint32_t len) {
    struct pi_span *s = len >= 0x4000 ? &spans_big[n_big++ % BIG_SPANS] : &spans_small[n_small++ % SMALL_SPANS];
    *s = (struct pi_span){ dram, len, rom, ++pi_seq };
    for (int i = 0; i < 4; i++)
        if (dram < hits[i].hi && dram + len > hits[i].lo) hits[i].lo = hits[i].hi = 0;
}

// The newest span of ring r (n written, size m) holding dram, searched newest first; the spans newer than it narrow
// [*lo, *hi), which must hold no newer DMA
static const struct pi_span *newest(const struct pi_span *r, uint32_t n, uint32_t m, uint32_t dram, uint32_t *lo, uint32_t *hi) {
    for (uint32_t k = 0; k < m && k < n; k++) {
        const struct pi_span *s = &r[(n - 1 - k) % m];
        if (dram - s->dram < s->len) return s;
        if (s->dram + s->len <= dram) { if (s->dram + s->len > *lo) *lo = s->dram + s->len; }
        else if (s->dram < *hi) *hi = s->dram;
    }
    return 0;
}

uint32_t pi_ledger_rom(uint32_t dram) {
    for (int i = 0; i < 4; i++)
        if (dram - hits[i].lo < hits[i].hi - hits[i].lo) return dram + hits[i].delta;
    uint32_t lo_s = 0, hi_s = ~0u, lo_b = 0, hi_b = ~0u;
    const struct pi_span *s = newest(spans_small, n_small, SMALL_SPANS, dram, &lo_s, &hi_s);
    const struct pi_span *b = newest(spans_big, n_big, BIG_SPANS, dram, &lo_b, &hi_b);
    const struct pi_span *best = s && (!b || s->seq > b->seq) ? s : b;
    if (!best) return ~0u;
    uint32_t lo = best->dram, hi = best->dram + best->len;  // and the other ring's newer spans narrow it too
    if (lo_s > lo) lo = lo_s;
    if (lo_b > lo) lo = lo_b;
    if (hi_s < hi) hi = hi_s;
    if (hi_b < hi) hi = hi_b;
    hits[next_hit] = (typeof(hits[0])){ lo, hi, best->rom - best->dram };
    next_hit = (next_hit + 1) & 3;
    return best->rom + (dram - best->dram);
}
