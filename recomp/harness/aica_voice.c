// Voices onto AICA channels: a note of an ADPCM sample plays its cache entry in place, its keys and levels queued a
// lead behind; anything else streams through a ring a lead ahead of the play position. Details: docs/n64_sample_cache.md
#include <string.h>

#include "aica_cache.h"
#include "aica_voice.h"

#define BLOCK 16u  // samples per 32-byte transfer

void adpcm_frames_fast(int16_t *last, const uint8_t *in, uint32_t n, const int16_t *codebook, uint32_t book_id, int16_t *out);
#define TAIL (AV_RING / 4)  // silence after a finished note, to cover the lists until its key-off
#define NONE (~0u)
#define ZERO_AHEAD 256u  // silence past a growing entry's end, for a play position that outruns the list
#define MAXPV 96u
#define MAXGAP 8u  // frames a list may skip that the cache decodes from the sample's source itself
#define STAGE 8192u  // samples of decoded frames a list may send to sound RAM
#define A_INIT 1u
#define A_LOOP 2u

struct voice {
    uint32_t id, m_w, m_f, r_w, gl, gr, freq_reg, tl, dipan, rate;  // rate: samples/s it consumes
    uint32_t n, last_n, end, drain_lists, drain_max;  // n: samples queued this list; end: where a finished note stops
    uint32_t seg, seg_n, g;  // the FIFO samples still without their envelope, and the gain the last ones ended at
    unsigned long long cp, c;  // sum of cnt * pitch and of cnt this list: the rate the samples were consumed at
    struct ac_entry *e;  // resident: the entry it plays, from position j0 on
    uint32_t f0, idx, gframe;  // the note's first frame; the sample index its stream has reached; as pv.gframe
    uint16_t j0;
    int8_t ring;
    uint8_t used, playing, seen, draining, lost, fs, rom;
};

// The list's voices as the plan sees them: a voice id comes once per update the list holds (nead has three)
struct pv {
    uint32_t id, f0, gframe, gprev;  // gframe: one past the last frame the game's own ADPCM decoded for the note
    struct ac_entry *e;
    uint16_t j0;
    uint8_t fs, rom, decoded, gone, taken;  // decoded: the list decodes its last update; gone: goes on streamed
};

static struct voice voices[AV_CHANNELS];
static int16_t fifo[AV_CHANNELS][AV_MIRROR] __attribute__((aligned(32)));
static const int16_t silence[AV_MIRROR / 2] __attribute__((aligned(32)));
static uint8_t ring_used[AV_RINGS];
static uint32_t out_hz, ready, npv, lists, staged;
static struct pv pvs[MAXPV];
struct av_stats av_stats;

// 16 * log2(x) for x > 0, to 1/16
static uint32_t log2q4(uint32_t x) {
    static const uint8_t frac[16] = { 0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 14, 15 };
    uint32_t m = 31 - (uint32_t)__builtin_clz(x);
    uint32_t top = m >= 4 ? (x >> (m - 4)) & 15 : (x << (4 - m)) & 15;
    return 16 * m + frac[top];
}

// AICA pitch: f = 44100 * 2^OCT * (1 + FNS/1024)
static uint32_t freq_reg(unsigned long long ratio_q16) {
    unsigned long long f = ratio_q16 * out_hz, base = 44100ull << 16;
    int oct = 0;
    if (!f) return 8u << 11;
    while (f < base && oct > -8) base >>= 1, oct--;
    while (f >= 2 * base && oct < 7) base <<= 1, oct++;
    unsigned long long fns = f > base ? ((f - base) << 10) / base : 0;
    return ((uint32_t)oct & 15) << 11 | (uint32_t)(fns > 1023 ? 1023 : fns);
}

// DIPAN attenuates the quieter side in 3 dB steps; the louder side's gain is already in the samples, or in TL
static uint32_t pan(uint32_t l, uint32_t r, uint32_t was) {
    uint32_t big = l > r ? l : r, small = l > r ? r : l;
    if (big < 2) return was;
    uint32_t steps = small ? (2 * (log2q4(big) - log2q4(small)) + 8) / 16 : 15;
    if (steps > 15) steps = 15;
    return !steps ? 0 : l > r ? 0x10 | steps : steps;
}

// TL attenuates in 0.375 dB steps, close enough to 16 a halving
static uint32_t tl_of(uint32_t g) {
    if (g < 2) return 255;
    uint32_t q = log2q4(g);
    return q >= 240 ? 0 : 240 - q;
}

static uint32_t pending(const struct voice *v) { return (v->m_w - v->m_f) & (AV_MIRROR - 1); }

// The lead to keep in the ring, in samples, never more than the FIFO can stage
static uint32_t lead(const struct voice *v) {
    uint32_t n = (uint32_t)((unsigned long long)v->rate * AV_LEAD_MS / 1000);
    n = (n + BLOCK - 1) & ~(BLOCK - 1);
    return n < 2 * BLOCK ? 2 * BLOCK : n > AV_MIRROR / 2 ? AV_MIRROR / 2 : n;
}

static struct voice *find(uint32_t id) {
    for (int i = 0; i < AV_CHANNELS; i++)
        if (voices[i].used && !voices[i].draining && voices[i].id == id) return &voices[i];
    return 0;
}

static struct voice *alloc(uint32_t id, int res) {
    int ring = -1;
    if (!res) {
        for (uint32_t k = 0; k < AV_RINGS && ring < 0; k++)
            if (!ring_used[k]) ring = (int)k;
        if (ring < 0) return 0;
    }
    for (int i = 0; i < AV_CHANNELS; i++) {
        struct voice *v = &voices[i];
        if (v->used || !aica_hw_idle((uint32_t)i)) continue;
        memset(v, 0, sizeof *v);
        v->used = 1, v->id = id, v->tl = 256, v->ring = (int8_t)ring, v->f0 = NONE;
        if (ring >= 0) {
            ring_used[ring] = 1;
            aica_hw_setup((uint32_t)i, AV_RING_BASE + (uint32_t)ring * AV_RING * 2);
        }
        return v;
    }
    return 0;
}

static void release(struct voice *v) {
    if (v->ring >= 0) ring_used[v->ring] = 0;
    if (v->e) ac_unref(v->e);
    v->used = 0, v->e = 0;
}

// The FIFO's whole blocks to the ring at r_w, split where either wraps
static void flush(struct voice *v) {
    uint32_t ch = (uint32_t)(v - voices), n = pending(v) & ~(BLOCK - 1);
    while (n) {
        uint32_t k = n, a = AV_MIRROR - v->m_f, b = AV_RING - v->r_w;
        if (k > a) k = a;
        if (k > b) k = b;
        aica_hw_ring_write(ch, v->r_w, &fifo[ch][v->m_f], 2 * k);
        av_stats.words += k / 2;
        v->m_f = (v->m_f + k) & (AV_MIRROR - 1);
        v->r_w = (v->r_w + k) & (AV_RING - 1);
        n -= k;
    }
}

void av_list_begin(uint32_t hz) {
    if (!ready) { aica_hw_init(); ac_init(AV_ARENA_BASE, AV_ARENA_END); ready = 1; }
    ac_tick();
    out_hz = hz;
    for (uint32_t i = 0; i < npv; i++)  // an entry the plan opened for a note that never came
        if (pvs[i].e && !pvs[i].taken) ac_unref(pvs[i].e);
    npv = staged = 0;
    lists++;
}

static int usable(const struct av_src *src) { return src && src->ok && src->keyed; }

// The update can play from e: silence only from an entry that ended, which loops zeros past its end
static int follows(const struct av_src *src, const struct ac_entry *e) {
    return src && src->ok && (!src->silent || (e && e->state == AC_FINAL && e->how == AC_BY_END));
}

// The frame index of address at (in the ROM if arom, else RDRAM) within the sample whose frame 0 is at f0, or -1
static long at_frame(uint32_t f0, uint8_t rom, uint8_t fs, uint32_t at, int arom) {
    if (f0 == NONE || at == NONE || arom != rom || at < f0 || (at - f0) % fs) return -1;
    return (long)((at - f0) / fs);
}

static int same_frame(uint8_t rom, uint32_t at, uint32_t ram, uint8_t fs) {
    uint8_t a[9], b[9];
    if (ram == NONE || !aa_frame_src(rom, at, a, fs)) return 0;
    aa_bytes(ram, b, fs);
    for (uint32_t k = 0; k < fs; k++)
        if (a[k] != b[k]) return 0;
    return 1;
}

// An ADPCM command's first frame in its note's sample, found once: one with no frames outputs only its state, so it sits at
// the game's next frame; others where their bytes match the sample's, first where the game should be, then the PI ledger's
static long cmd_frame(uint32_t f0, uint8_t rom, uint8_t fs, uint32_t gframe, uint32_t *loopf, const struct av_src *src,
                      struct av_adpcm *d) {
    if (d->f != AV_UNRESOLVED) return d->f;
    d->f = -1;
    if (f0 == NONE) return -1;
    if (!d->frames && d->ram == NONE && !(d->flags & A_LOOP)) return d->f = (int32_t)gframe;
    uint32_t expect = d->flags & A_INIT ? 0 : gframe;
    for (const struct av_adpcm *c = src->adpcm; c < d && !(d->flags & A_INIT); c++)
        if (c->f >= 0 && !(c->flags & A_LOOP)) expect = (uint32_t)c->f + c->frames;
    for (uint32_t k = 0; !(d->flags & A_LOOP) && k <= MAXGAP + 1; k++) {
        uint32_t f = !k ? expect : k == 1 ? expect - 1 : expect + k - 1;
        if (f < 0x80000000u && same_frame(rom, f0 + f * fs, d->ram, fs)) return d->f = (int32_t)f;
    }
    if (!(d->flags & A_LOOP)) return -1;  // not the note's: another voice's output read stale
    if (loopf && *loopf != NONE && same_frame(rom, f0 + *loopf * fs, d->ram, fs)) return d->f = (int32_t)*loopf;
    int r;
    long f = at_frame(f0, rom, fs, aa_frame_at(d->ram, &r), r);
    if (f >= 0 && loopf) *loopf = (uint32_t)f;
    return d->f = (int32_t)f;
}

static uint32_t book_of(struct av_adpcm *d) {
    if (!d->book && d->book_len) d->book = aa_book_hash(d->book_addr, d->book_len);
    return d->book;
}

// The sample index of the list's stream sample k: its run starts at output index j of an ADPCM command whose first
// input frame is F, so at sample 16 F + j - 16
static int idx_at(uint32_t f0, uint8_t rom, uint8_t fs, uint32_t gframe, uint32_t *loopf, struct av_src *src, uint32_t k,
                  uint32_t *idx) {
    for (uint32_t g = 0; g < src->nseg; g++) {
        const struct av_seg *t = &src->seg[g];
        if (k - t->at >= t->n) continue;
        long f = t->adpcm >= 0 ? cmd_frame(f0, rom, fs, gframe, loopf, src, &src->adpcm[t->adpcm]) : -1;
        if (f < 0) return 0;
        *idx = 16 * (uint32_t)f + t->j - 16 + (k - t->at);
        return 1;
    }
    return 0;
}

// Decoded frames waiting for their transfer to sound RAM: one half a list, as a list's transfers run until the next
static int16_t stage[2][STAGE] __attribute__((aligned(32)));

// Grows the entry by this list's ADPCM frames, from its RDRAM bytes or, when the list skipped one, the sample's source;
// commands not the note's don't count. 1: the entry holds them all; -1: not in its space; 0: not reachable
static int grow(const struct pv *p, struct av_src *src) {
    struct ac_entry *e = p->e;
    const struct av_adpcm *bk = 0;  // a command that decodes new frames, with the entry's codebook
    uint32_t target = e->frames, mapped = 0;
    for (uint32_t q = 0; q < src->nadpcm; q++) {
        long f = (src->adpcm[q].flags & A_LOOP) ? -1 : cmd_frame(p->f0, p->rom, p->fs, p->gframe, &e->lframe, src, &src->adpcm[q]);
        if (f < 0) continue;
        mapped++;
        if ((uint32_t)f > target + MAXGAP) { av_stats.why_gap++; return 0; }
        if ((uint32_t)f + src->adpcm[q].frames <= target) continue;
        if (book_of(&src->adpcm[q]) != e->book) { av_stats.why_book++; return 0; }
        target = (uint32_t)f + src->adpcm[q].frames, bk = &src->adpcm[q];
    }
    uint32_t idx;
    if (!mapped && src->nadpcm && !(src->adpcm[0].flags & A_LOOP)) {
        av_stats.why_unmapped++;
        return 0;
    }
    if (src->nseg && !idx_at(p->f0, p->rom, p->fs, p->gframe, &e->lframe, src, 0, &idx)) { av_stats.why_head++; return 0; }
    if (target <= e->frames || !bk) return 1;
    if (16 * (target + 1) + BLOCK > ac_room(e)) {  // twice the space in place, if the arena after it is free
        uint32_t want = 16 * (target + 1) + BLOCK;
        if (!ac_grow(e, want > e->cap ? want : e->cap)) return -1;
        for (int i = 0; i < AV_CHANNELS; i++)  // channels on it loop the silence at its new end
            if (voices[i].used && !voices[i].draining && voices[i].e == e)
                aica_hw_loop((uint32_t)i, ac_room(e) - voices[i].j0, ac_room(e) + AC_TAIL - voices[i].j0);
    }
    if (staged + 16 * (target - e->frames) > STAGE) { av_stats.why_stage++; return 0; }
    static int16_t book[256];  // off the RSP fiber's 4 KB stack
    int16_t *out = &stage[lists & 1][staged];
    uint32_t from = e->len, n = 0;
    aa_book(bk->book_addr, bk->book_len, book);
    static uint8_t enc[64 * 9];  // a run of frames' bytes, decoded in one call
    for (int short_run = 0; e->frames < target && !short_run;) {
        uint32_t nb = 0;
        for (; nb < 64 && e->frames + nb < target; nb++) {
            uint32_t k = e->frames + nb, q = 0;
            for (; q < src->nadpcm; q++) {
                long f = (src->adpcm[q].flags & A_LOOP) ? -1 : src->adpcm[q].f;
                if (f >= 0 && k - (uint32_t)f < src->adpcm[q].frames) { aa_bytes(src->adpcm[q].ram + (k - (uint32_t)f) * p->fs, enc + nb * p->fs, p->fs); break; }
            }
            if (q == src->nadpcm && !aa_frame_src(p->rom, p->f0 + k * p->fs, enc + nb * p->fs, p->fs)) { short_run = 1; break; }
        }
        if (p->fs == 9) adpcm_frames_fast(e->last, enc, nb, book, bk->book, out + n);
        else for (uint32_t i = 0; i < nb; i++) ac_decode_frame(e->last, enc + 5 * i, 1, book), memcpy(out + n + 16 * i, e->last, sizeof e->last);
        n += 16 * nb, e->frames += nb;
    }
    if (n) aica_hw_write(e->off + 2 * from, out, 2 * n);
    staged += n;
    e->len = 16 * (e->frames + 1);
    av_stats.decoded_frames += n / 16;
    if (e->len + ZERO_AHEAD <= ac_room(e)) aica_hw_write(e->off + 2 * e->len, silence, 2 * ZERO_AHEAD);
    return e->frames == target;
}

static struct pv *plan_of(uint32_t id, int init) {
    if (!init)
        for (uint32_t i = npv; i-- > 0;)
            if (pvs[i].id == id) return &pvs[i];
    if (npv == MAXPV) return 0;
    struct pv *p = &pvs[npv++];
    memset(p, 0, sizeof *p);
    p->id = id, p->f0 = NONE, p->decoded = 1;
    if (init) return p;
    const struct voice *v = find(id);
    if (!v)  // a voice keyed off as late at its entry's loop goes on streamed, its ADPCM state rebuilt
        for (int i = 0; i < AV_CHANNELS && !v; i++)
            if (voices[i].used && voices[i].draining && voices[i].lost && voices[i].e && voices[i].id == id) v = &voices[i];
    if (v && v->e) {
        p->e = v->e, p->f0 = v->f0, p->j0 = v->j0, p->fs = v->fs, p->rom = v->rom, p->taken = 1;
        p->decoded = 0, p->gone = v->lost, p->gframe = v->gframe;
    }
    return p;
}

// A final entry plays an update only if one of its ADPCM commands is the note's; another sample on the voice is not
static int maps(const struct pv *p, struct av_src *src, struct ac_entry *e) {
    for (uint32_t q = 0; q < src->nadpcm; q++)
        if (cmd_frame(p->f0, p->rom, p->fs, p->gframe, &e->lframe, src, &src->adpcm[q]) >= 0) return 1;
    return 0;
}

// An entry ended by a CLEAR whose note decodes frames beyond that end
static int past_end(const struct pv *p, struct av_src *src, struct ac_entry *e) {
    if (e->state != AC_FINAL || e->how != AC_BY_END) return 0;
    for (uint32_t q = 0; q < src->nadpcm; q++) {
        if (src->adpcm[q].flags & A_LOOP) continue;
        long f = cmd_frame(p->f0, p->rom, p->fs, p->gframe, &e->lframe, src, &src->adpcm[q]);
        if (f >= 0 && (uint32_t)f + src->adpcm[q].frames > e->frames) return 1;
    }
    return 0;
}

// One past the last frame the list's last ADPCM command for the note decodes: what its RDRAM state then holds
static uint32_t game_frame(uint32_t f0, uint8_t rom, uint8_t fs, uint32_t *loopf, struct av_src *src, uint32_t was) {
    for (uint32_t q = src ? src->nadpcm : 0; q-- > 0;) {
        long f = cmd_frame(f0, rom, fs, was, loopf, src, &src->adpcm[q]);
        if (f >= 0) return (uint32_t)f + src->adpcm[q].frames;
    }
    return was;
}

int av_plan(uint32_t id, int init, uint32_t n, struct av_src *src) {
    (void)n;
    struct pv *p = plan_of(id, init);
    if (!p) return AV_DECODE;
    if (init && usable(src)) {
        p->e = ac_open(src->key, src->rom, src->book, src->f0);
        p->f0 = src->f0, p->j0 = src->j0, p->fs = src->fs, p->rom = src->rom;
        av_stats.cold_refused += !p->e;
    } else if (init) {
        av_stats.cold_foreign += !src || !src->ok;
        av_stats.cold_unkeyed += src && src->ok && !src->keyed;
    }
    struct ac_entry *e = p->e;
    int ok = follows(src, e);
    if (ok && e && e->state == AC_FINAL && !src->silent && src->nadpcm && !maps(p, src, e)) ok = 0, av_stats.why_unmapped++;
    if (ok && e && !src->silent && past_end(p, src, e) && ac_reopen(e))  // the CLEAR wasn't the sample's end
        for (int i = 0; i < AV_CHANNELS; i++)
            if (voices[i].used && !voices[i].draining && voices[i].e == e)
                aica_hw_loop((uint32_t)i, ac_room(e) - voices[i].j0, ac_room(e) + AC_TAIL - voices[i].j0);
    int r, reach = e && e->state == AC_PART && ok && !p->gone ? grow(p, src) : 0;
    if (reach < 0) ac_outgrown(e), av_stats.outran++;
    if (!e || (p->gone && p->decoded)) r = AV_DECODE;
    else if (!p->gone && ok && (e->state == AC_FINAL || reach > 0)) r = AV_SKIP;
    else {
        av_stats.lost += !p->gone;
        av_stats.why_foreign += !p->gone && !ok;
        p->gone = 1;
        r = p->decoded ? AV_DECODE : AV_PATCH;
        if (init && !p->taken) ac_unref(e), p->e = 0, r = AV_DECODE, av_stats.cold_dropped++;
    }
    p->decoded = r != AV_SKIP;
    av_stats.streamed_gone += r != AV_SKIP && e && p->gone;
    av_stats.streamed_cold += r != AV_SKIP && !e;
    av_stats.streamed_silent += r != AV_SKIP && !e && src && src->silent;
    p->gprev = p->gframe, p->gframe = game_frame(p->f0, p->rom, p->fs, e ? &e->lframe : 0, src, p->gframe);
    return r;
}

// The state an ADPCM command loads is the last frame the game decoded for the note, G - 1, at entry positions 16 G to
// 16 G + 15; normally G is the command's first frame F, but a list may skip or repeat a frame
int av_state(uint32_t id, uint32_t frame, int rom, int16_t out[16]) {
    const struct pv *v = 0;
    for (uint32_t i = npv; i-- > 0 && !v;)
        if (pvs[i].id == id) v = &pvs[i];
    long f = v && v->e ? at_frame(v->f0, v->rom, v->fs, frame, rom) : -1;
    if (f >= 0 && v->gprev) f = (long)v->gprev;
    if (f < 1 || 16 * (uint32_t)f + 16 > v->e->len) return 0;
    if ((uint32_t)f == v->e->frames) memcpy(out, v->e->last, sizeof v->e->last);  // decoded this list, maybe not yet sent
    else for (uint32_t k = 0; k < 16; k++) out[k] = ac_sample(v->e, 16 * (uint32_t)f + k);
    av_stats.patches++;
    return 1;
}



// Every channel on a newly final entry loops it now, unless its play position may already be past the loop's end
static void finalize(struct ac_entry *e) {
    for (int i = 0; i < AV_CHANNELS; i++) {
        struct voice *u = &voices[i];
        if (!u->used || u->draining || u->e != e || u->lost) continue;
        uint32_t margin = (uint32_t)((unsigned long long)u->rate * AV_LEAD_MS / 2000);
        if (u->idx + 16 <= e->lea + margin) aica_hw_loop((uint32_t)i, e->lsa - u->j0, e->lea - u->j0);
        else u->lost = 1, av_stats.late++;
    }
}

// A resident note's list: the entry grows by what its ADPCM commands decode, and turns final at a restart or the end
// The sample index just past stream sample k - 1: where the stream stood before k (v->idx at k = 0)
static uint32_t idx_before(const struct voice *v, struct av_src *src, uint32_t k, int *ok) {
    uint32_t idx = v->idx;
    *ok = !k || idx_at(v->f0, v->rom, v->fs, v->gframe, v->e ? &v->e->lframe : 0, src, k - 1, &idx);
    return k ? idx + 1 : idx;
}

static void resident(struct voice *v, uint32_t n, struct av_src *src) {
    struct ac_entry *e = v->e;
    int ok;
    if (src && e->state == AC_PART) {
        uint32_t l, end;
        if (src->wrap != NONE && idx_at(v->f0, v->rom, v->fs, v->gframe, &e->lframe, src, src->wrap, &l)) {
            end = idx_before(v, src, src->wrap, &ok);
            if (ok && l < end) ac_final_loop(e, l + 16, end + 16);
        } else if (src->end != NONE) {
            end = idx_before(v, src, src->end, &ok);
            if (ok) ac_final_end(e, end + 16);
        }
        if (e->state == AC_FINAL) finalize(e);
    }
    uint32_t next = idx_before(v, src ? src : &(struct av_src){ 0 }, n, &ok);
    v->idx = ok ? next : v->idx + n;
    v->gframe = game_frame(v->f0, v->rom, v->fs, &e->lframe, src, v->gframe);
    av_stats.res_samples += n;
}

// The entry av_plan opened for this note
static struct ac_entry *take(uint32_t id) {
    for (uint32_t i = npv; i-- > 0;)
        if (pvs[i].id == id && pvs[i].e && !pvs[i].taken) { pvs[i].taken = 1; return pvs[i].e; }
    return 0;
}

void av_voice(uint32_t id, int init, const int16_t *s, uint32_t n, uint32_t cnt, uint32_t pitch, struct av_src *src) {
    struct voice *v = find(id);
    if (v && init) {
        v->id = 0, v->seen = 0;  // the old note plays out from the end of this list; the new one gets its own channel
        v = 0;
    }
    if (!v) {
        struct ac_entry *e = init && !s ? take(id) : 0;
        if (init && s && (e = take(id))) ac_unref(e), e = 0;
        if (!(v = alloc(id, e != 0))) {
            if (e) ac_unref(e);
            av_stats.dropped++;
            return;
        }
        av_stats.resumed += !init;
        if (e) v->e = e, v->f0 = src->f0, v->j0 = src->j0, v->fs = src->fs, v->rom = src->rom, v->idx = src->j0 - 16u;
    }
    if (v->e && s) {  // the plan handed it off: its key-off goes out a lead late, as the ring's first samples do
        v->lost = 1, v->id = 0, v->seen = 1;
        if (!(v = alloc(id, 0))) { av_stats.dropped++; return; }
        av_stats.resumed++;
    }
    v->seen = 1;
    v->cp += (unsigned long long)(cnt >> 1) * pitch;
    v->c += cnt >> 1;
    if (v->e) { resident(v, n, src); return; }
    if (!s) return;
    if (pending(v) + n >= AV_MIRROR - BLOCK) { av_stats.overruns++; return; }
    v->n += n;
    if (!v->seg_n) v->seg = v->m_w;
    v->seg_n += n;
    int16_t *m = fifo[v - voices];
    for (uint32_t done = 0; done < n;) {
        uint32_t room = AV_MIRROR - v->m_w, k = n - done < room ? n - done : room;
        memcpy(m + v->m_w, s + done, 2 * k);
        v->m_w = (v->m_w + k) & (AV_MIRROR - 1);
        done += k;
    }
}

// The queued samples under the envelope, ramped from the last gain to g1 as the RSP's mixer ramps it, so the gain
// reaches the AICA with its own samples rather than a ring lead ahead of them
static void envelope(struct voice *v, uint32_t g1) {
    int16_t *m = fifo[v - voices];
    uint32_t n = v->seg_n, at = v->seg;
    if (n && !(v->g == g1 && g1 >= 32768)) {
        int32_t g = (int32_t)v->g * 32768, step = ((int32_t)g1 - (int32_t)v->g) * 32768 / (int32_t)n;
        while (n) {  // in runs up to the FIFO's wrap; a held gain skips the ramp
            uint32_t k = AV_MIRROR - at < n ? AV_MIRROR - at : n;
            int16_t *s = m + at, *e = s + k;
            if (!step) {
                int32_t c = g >> 15;
                for (; s < e; s++) *s = (int16_t)((*s * c) >> 15);
            } else {
                for (; s < e; s++) g += step, *s = (int16_t)((*s * (g >> 15)) >> 15);
            }
            at = (at + k) & (AV_MIRROR - 1), n -= k;
        }
    }
    v->g = g1, v->seg_n = 0;
}

void av_gain(uint32_t id, uint32_t l, uint32_t r, uint32_t wet) {
    (void)wet;
    struct voice *v = find(id);
    if (!v) return;
    l = l > 32768 ? 32768 : l, r = r > 32768 ? 32768 : r;
    v->gl = l, v->gr = r;
    if (!v->e) envelope(v, l > r ? l : r);
}

// k samples of silence over the ring from p; returns where they end
static uint32_t ring_silence(uint32_t ch, uint32_t p, uint32_t k) {
    while (k) {
        uint32_t n = k < AV_RING - p ? k : AV_RING - p;
        if (n > AV_MIRROR / 2) n = AV_MIRROR / 2;
        aica_hw_ring_write(ch, p, silence, 2 * n);
        p = (p + n) & (AV_RING - 1);
        k -= n;
    }
    return p;
}

// A resident note's list: its pitch and level, key-on in its first list, key-off once it has left
static void resident_end(struct voice *v, uint32_t ch, uint32_t *active) {
    if (v->draining) {
        if (aica_hw_idle(ch)) release(v);
        return;
    }
    int ended = !v->seen;
    *active += !ended && !v->lost;
    v->seen = 0;
    uint32_t fr = v->freq_reg, tl = tl_of(v->gl > v->gr ? v->gl : v->gr), dipan = pan(v->gl, v->gr, v->dipan);
    if (v->c) {
        unsigned long long ratio = v->cp / v->c;
        v->rate = (uint32_t)(ratio * out_hz >> 16);
        fr = freq_reg(ratio);
        v->cp = v->c = 0;
    }
    const struct ac_entry *e = v->e;
    if (!v->playing) {
        int fin = e->state == AC_FINAL;
        uint32_t lsa = fin ? e->lsa : ac_room(e), lea = fin ? e->lea : ac_room(e) + AC_TAIL;
        aica_hw_event(&(struct av_ev){ ch, AV_EV_START, e->off + 2u * v->j0, lsa - v->j0, lea - v->j0, fr, tl, dipan });
        v->playing = 1;
        av_stats.keyons++, av_stats.res_keyons++;
    } else if (fr != v->freq_reg || tl != v->tl || dipan != v->dipan) {
        aica_hw_event(&(struct av_ev){ ch, AV_EV_PARAMS, 0, 0, 0, fr, tl, dipan });
    }
    v->freq_reg = fr, v->tl = tl, v->dipan = dipan;
    if (ended || v->lost) {
        aica_hw_event(&(struct av_ev){ ch, AV_EV_STOP, 0, 0, 0, 0, 0, 0 });
        v->draining = 1;
    }
}

void av_list_end(void) {
    unsigned long long on = 0, off = 0;
    uint32_t active = 0;
    for (int i = 0; i < AV_CHANNELS; i++) {
        struct voice *v = &voices[i];
        if (!v->used) continue;
        if (v->e) { resident_end(v, (uint32_t)i, &active); continue; }
        if (v->draining) {
            uint32_t left = (v->end - aica_hw_pos((uint32_t)i)) & (AV_RING - 1);
            int done = left > AV_RING - TAIL;  // the play position is into the silence after the note
            if (done || ++v->drain_lists > v->drain_max) {
                off |= 1ull << i;
                release(v);
                if (!done) av_stats.cut += left;
            }
            continue;
        }
        int ended = !v->seen;
        active += !ended;
        v->seen = 0;
        if (v->n) v->last_n = v->n;
        v->n = 0;
        if (v->seg_n) envelope(v, v->g);  // samples no mixer claimed hold the last gain
        if (ended && (pending(v) & (BLOCK - 1))) {  // zero the rest of its last block, so all of it reaches the ring
            uint32_t k = BLOCK - (pending(v) & (BLOCK - 1));
            memset(&fifo[i][v->m_w], 0, 2 * k);
            v->m_w = (v->m_w + k) & (AV_MIRROR - 1);
        }
        if (v->c) {
            unsigned long long ratio = v->cp / v->c;
            v->rate = (uint32_t)(ratio * out_hz >> 16);
            uint32_t fr = freq_reg(ratio), tl = 0, dipan = pan(v->gl, v->gr, v->dipan);
            if (fr != v->freq_reg || tl != v->tl || dipan != v->dipan) {
                aica_hw_params((uint32_t)i, fr, tl, dipan);
                v->freq_reg = fr, v->tl = tl, v->dipan = dipan;
            }
            v->cp = v->c = 0;
        }
        uint32_t pos = 0;
        if (!v->playing) {
            uint32_t have = pending(v) & ~(BLOCK - 1), want = lead(v);
            if (!have && ended) {
                release(v);
                continue;
            }
            if (have < want) v->r_w = ring_silence((uint32_t)i, v->r_w, want - have);  // every note starts a lead late
            flush(v);
            on |= 1ull << i;
            v->playing = 1;
            av_stats.keyons++;
        } else {
            pos = aica_hw_pos((uint32_t)i);
            uint32_t ahead = (v->r_w - pos) & (AV_RING - 1);
            if (ahead < BLOCK || ahead > AV_RING - AV_RING / 4) {
                av_stats.underruns++;  // the skipped span plays as silence, not a stale lap of the ring
                v->r_w = ring_silence((uint32_t)i, (pos + BLOCK) & (AV_RING - 1) & ~(BLOCK - 1), lead(v));
            } else if (ahead + pending(v) > AV_RING - 2 * BLOCK) {
                av_stats.overruns++;
                v->m_f = v->m_w & ~(BLOCK - 1);
            }
            flush(v);
        }
        if (ended) {
            uint32_t ahead = (v->r_w - pos) & (AV_RING - 1);
            v->end = v->r_w;
            ring_silence((uint32_t)i, v->r_w, AV_RING - ahead >= TAIL + BLOCK ? TAIL : 0);  // never over unplayed samples
            v->draining = 1, v->drain_lists = 0, v->drain_max = 2 * ahead / (v->last_n ? v->last_n : 1) + 4;  // lists come in bursts
        }
    }
    aica_hw_flush();
    aica_hw_keys(on, off);
    aica_hw_list_done();
    av_stats.active = active;
}
