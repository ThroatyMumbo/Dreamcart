// Sound-RAM sample entries: first-fit in 32-byte units, least recently used evicted first, referenced entries pinned.
#include <string.h>

#include "aica_cache.h"
#include "aica_voice.h"

#define TAB 512u
#define HINT0 (16384u * 2)  // a new entry's space in bytes; doubled after a note outruns it
#define MOST (2u * (AC_MAX_SAMPLES + AC_TAIL))

static struct ac_entry ents[AC_ENTRIES];
static int16_t tab[TAB], order[AC_ENTRIES];
static uint32_t norder, base, end, now;
static const int16_t zeros[32] __attribute__((aligned(32)));
struct ac_stats ac_stats;

static uint32_t slot_of(unsigned long long key) { return (uint32_t)(key ^ key >> 29) & (TAB - 1); }

void ac_init(uint32_t arena_off, uint32_t arena_end) {
    memset(ents, 0, sizeof ents);
    memset(tab, 0xFF, sizeof tab);
    memset(&ac_stats, 0, sizeof ac_stats);
    norder = 0, base = arena_off, end = arena_end, now = 1;
}

struct ac_entry *ac_find(unsigned long long key) {
    for (uint32_t s = slot_of(key), k = 0; k < TAB; k++, s = (s + 1) & (TAB - 1)) {
        if (tab[s] == -1) return 0;
        if (tab[s] >= 0 && ents[tab[s]].key == key) return &ents[tab[s]];
    }
    return 0;
}

// Its sound RAM back to the arena; the key stays known, with its size hint
static void unplace(struct ac_entry *e) {
    if (!e->cap) return;
    int16_t i = (int16_t)(e - ents);
    for (uint32_t k = 0; k < norder; k++)
        if (order[k] == i) { memmove(&order[k], &order[k + 1], (norder - k - 1) * sizeof order[0]); norder--; break; }
    ac_stats.bytes -= e->cap;
    if (e->state == AC_FINAL) ac_stats.finals--;
    if (e->state == AC_PART) ac_stats.parts--;
    if (e->state == AC_FINAL || e->state == AC_PART) e->state = AC_IDLE;
    e->cap = 0, e->len = 0;
}

// The middle of the largest gap that holds bytes: the entry before can still grow, and so can the new one
static uint32_t gap(uint32_t bytes, uint32_t *at) {
    uint32_t p = base, best = ~0u, best_len = 0;
    for (uint32_t k = 0; k <= norder; k++) {
        uint32_t q = k < norder ? ents[order[k]].off : end;
        if (q >= p && q - p >= bytes && q - p > best_len) best = p, best_len = q - p, *at = k;
        if (k < norder) p = ents[order[k]].off + ents[order[k]].cap;
    }
    return best == ~0u ? best : best + (((best_len - bytes) / 2) & ~31u);
}

static struct ac_entry *victim(void) {
    struct ac_entry *v = 0;
    for (uint32_t k = 0; k < norder; k++) {
        struct ac_entry *e = &ents[order[k]];
        if (!e->refs && (!v || e->lru < v->lru)) v = e;
    }
    return v;
}

static int place(struct ac_entry *e, uint32_t bytes) {
    uint32_t at, off;
    while ((off = gap(bytes, &at)) == ~0u) {
        struct ac_entry *v = victim();
        if (!v) { ac_stats.no_room++; return 0; }
        unplace(v);
        ac_stats.evicted++;
    }
    memmove(&order[at + 1], &order[at], (norder - at) * sizeof order[0]);
    order[at] = (int16_t)(e - ents), norder++;
    e->off = off, e->cap = bytes;
    ac_stats.bytes += bytes;
    return 1;
}

// A slot for a new key: a free one, else the least recently used one nothing references
static struct ac_entry *claim(unsigned long long key) {
    struct ac_entry *e = 0;
    for (uint32_t i = 0; i < AC_ENTRIES && !e; i++)
        if (!ents[i].state) e = &ents[i];
    if (!e) {
        for (uint32_t i = 0; i < AC_ENTRIES; i++) {
            struct ac_entry *c = &ents[i];
            if (!c->refs && (!e || c->lru < e->lru)) e = c;
        }
        if (!e) return 0;
        unplace(e);
        for (uint32_t s = slot_of(e->key);; s = (s + 1) & (TAB - 1))
            if (tab[s] == (int16_t)(e - ents)) { tab[s] = -2; break; }
        ac_stats.entries--;
    }
    memset(e, 0, sizeof *e);
    e->key = key, e->state = AC_IDLE, e->hint = HINT0, e->lru = now;
    uint32_t s = slot_of(key);
    while (tab[s] >= 0) s = (s + 1) & (TAB - 1);
    tab[s] = (int16_t)(e - ents);
    ac_stats.entries++;
    return e;
}

struct ac_entry *ac_open(unsigned long long key, int rom, uint32_t book, uint32_t at) {
    struct ac_entry *e = ac_find(key);
    if (!e && !(e = claim(key))) return 0;
    if (e->state == AC_NEVER || e->outgrown) return 0;
    if (e->state == AC_IDLE) {
        if (!place(e, e->hint)) return 0;
        e->state = AC_PART, e->len = 16, e->frames = 0, e->rom = (uint8_t)rom, e->how = 0, e->book = book, e->lframe = ~0u;
        e->at = at;
        memset(e->last, 0, sizeof e->last);
        ac_stats.parts++;
        aica_hw_write(e->off, zeros, 32);
        aica_hw_write(e->off + e->cap - 2 * AC_TAIL, zeros, 2 * AC_TAIL);
    }
    e->refs++, e->lru = now;
    return e;
}

// mupen's rdot and adpcm_compute_residuals (mupen64plus-rsp-hle/src/audio.c), so the cache decodes as the HLE does
static int32_t rdot(uint32_t n, const int16_t *x, const int16_t *y) {
    int32_t accu = 0;
    for (y += n; n; n--) accu += *x++ * *--y;
    return accu;
}

static void residuals(int16_t *dst, const int16_t *src, const int16_t *cb, const int16_t *last_samples) {
    const int16_t l1 = last_samples[0], l2 = last_samples[1];
    for (uint32_t i = 0; i < 8; i++) {
        int32_t accu = ((int32_t)src[i] << 11) + cb[i] * l1 + cb[8 + i] * l2 + rdot(i, cb + 8, src);
        dst[i] = (int16_t)sat_i16(accu >> 11);
    }
}

void ac_decode_frame(int16_t last[16], const uint8_t *in, int two_bit, const int16_t *book) {
    int16_t frame[16];
    unsigned scale = in[0] >> 4;
    const int16_t *cb = book + ((in[0] & 0xF) << 4);
    if (two_bit) {
        unsigned rshift = scale < 14 ? 14 - scale : 0;
        for (int i = 0; i < 4; i++) {
            uint8_t b = in[1 + i];
            frame[4 * i + 0] = (int16_t)((uint16_t)(b & 0xC0) << 8) >> rshift;
            frame[4 * i + 1] = (int16_t)((uint16_t)(b & 0x30) << 10) >> rshift;
            frame[4 * i + 2] = (int16_t)((uint16_t)(b & 0x0C) << 12) >> rshift;
            frame[4 * i + 3] = (int16_t)((uint16_t)(b & 0x03) << 14) >> rshift;
        }
    } else {
        unsigned rshift = scale < 12 ? 12 - scale : 0;
        for (int i = 0; i < 8; i++) {
            uint8_t b = in[1 + i];
            frame[2 * i] = (int16_t)((uint16_t)(b & 0xF0) << 8) >> rshift;
            frame[2 * i + 1] = (int16_t)((uint16_t)(b & 0x0F) << 12) >> rshift;
        }
    }
    residuals(last, frame, cb, last + 14);
    residuals(last + 8, frame + 8, cb, last + 6);
}

uint32_t ac_room(const struct ac_entry *e) { return e->cap / 2 - AC_TAIL; }

int ac_grow(struct ac_entry *e, uint32_t samples) {
    uint32_t want = (2 * (samples + AC_TAIL) + 31) & ~31u, k = 0;
    if (e->state != AC_PART || want <= e->cap || want > MOST + 32) return 0;
    while (k < norder && order[k] != (int16_t)(e - ents)) k++;
    if (k == norder) return 0;
    uint32_t next = k + 1 < norder ? ents[order[k + 1]].off : end;
    while (next - e->off < want && k + 1 < norder && !ents[order[k + 1]].refs) {  // an idle neighbor goes before a playing note does
        unplace(&ents[order[k + 1]]);
        ac_stats.evicted++;
        next = k + 1 < norder ? ents[order[k + 1]].off : end;
    }
    if (next - e->off < want) return 0;
    ac_stats.bytes += want - e->cap;
    e->cap = want;
    aica_hw_write(e->off + e->cap - 2 * AC_TAIL, zeros, 2 * AC_TAIL);
    ac_stats.grown++;
    return 1;
}

// A final entry keeps its whole space while notes still write past its end, then shrinks in place
static void shrink(struct ac_entry *e) {
    uint32_t cap = (2 * e->lea + 31) & ~31u;
    if (e->state != AC_FINAL || cap >= e->cap) return;
    ac_stats.bytes -= e->cap - cap;
    e->cap = cap;
}

void ac_unref(struct ac_entry *e) {
    if (e->refs) e->refs--;
    e->lru = now;
    if (e->refs) return;
    if (e->outgrown) unplace(e), e->outgrown = 0;
    else shrink(e);
}

struct ac_final_rec ac_finals[AC_FINAL_LOG];
uint32_t ac_nfinals;

static void final(struct ac_entry *e) {
    e->state = AC_FINAL;
    ac_stats.parts--, ac_stats.finals++;
    if (ac_nfinals < AC_FINAL_LOG)
        ac_finals[ac_nfinals++] = (struct ac_final_rec){ e->at, e->len, e->lsa, e->lea, e->frames, e->how, e->rom };
}

void ac_final_loop(struct ac_entry *e, uint32_t lsa, uint32_t lea) {
    if (e->state != AC_PART || lea > e->len || lea <= lsa + 16) return;
    e->len = lea, e->lsa = lsa, e->lea = lea, e->how = AC_BY_LOOP;
    final(e);
}

void ac_final_end(struct ac_entry *e, uint32_t len) {
    uint32_t tail = (len + 15) & ~15u;
    if (e->state != AC_PART || len > e->len || tail + 32 > ac_room(e)) return;
    aica_hw_write(e->off + 2 * tail, zeros, sizeof zeros);
    e->len = len, e->lsa = tail, e->lea = tail + 32, e->how = AC_BY_END;
    final(e);
}

int ac_reopen(struct ac_entry *e) {
    if (e->state != AC_FINAL || e->how != AC_BY_END) return 0;
    e->state = AC_PART, e->how = 0, e->len = 16 * (e->frames + 1), e->lsa = e->lea = 0;
    ac_stats.finals--, ac_stats.parts++, ac_stats.reopened++;
    aica_hw_write(e->off + e->cap - 2 * AC_TAIL, zeros, 2 * AC_TAIL);
    return 1;
}

void ac_outgrown(struct ac_entry *e) {
    if (e->outgrown) return;
    e->outgrown = 1;
    ac_stats.outgrown++;
    if (e->cap < MOST) e->hint = 2 * e->cap < MOST ? 2 * e->cap : (MOST + 31) & ~31u;
    else e->state = AC_NEVER, ac_stats.too_long++, ac_stats.parts--;
}

void ac_tick(void) { now++; }

int16_t ac_sample(const struct ac_entry *e, uint32_t pos) {
    if (e->state == AC_FINAL && pos >= e->lea) pos = e->lsa + (pos - e->lsa) % (e->lea - e->lsa);
    int16_t v;
    aica_hw_read(e->off + 2 * pos, &v, 2);
    return v;
}
