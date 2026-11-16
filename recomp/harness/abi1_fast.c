// SH-4 versions of the alist commands that dominate ABI1 and nead audio tasks, bit-exact with
// mupen64plus-rsp-hle alist.c (its copies are weakened at link).
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "alist.h"
#include "arithmetics.h"
#include "audio.h"
#include "hle_external.h"
#include "hle_internal.h"
#include "memory.h"

#include "htypes.h"  // sat_i16: clamp_s16 with one compare; sums wrap as mupen's int arithmetic does

static inline int32_t mixed(int32_t acc, int32_t src, int32_t gain) { return sat_i16(acc + ((src * gain) >> 15)); }

struct ramp { int64_t value, step, target; };

static inline int16_t ramp_step(struct ramp *r) {
    r->value += r->step;
    if (r->step <= 0 ? r->value <= r->target : r->value >= r->target) {
        r->value = r->target;
        r->step = 0;
    }
    return (int16_t)(r->value >> 16);
}

// Both ramps still: every remaining sample has the same gains, in mupen's sample order (ptr ^ S)
static void envmix_still(int16_t *dl, int16_t *dr, int16_t *wl, int16_t *wr, const int16_t *in, bool aux,
                         uint32_t ptr, uint32_t n, int32_t g0, int32_t g1, int32_t g2, int32_t g3) {
    uint32_t end = ptr + n;
    if (aux) {
        for (; ptr < end; ptr++) {
            uint32_t i = ptr ^ S;
            int32_t s = in[i];
            dl[i] = (int16_t)mixed(dl[i], s, g0);
            dr[i] = (int16_t)mixed(dr[i], s, g1);
            wl[i] = (int16_t)mixed(wl[i], s, g2);
            wr[i] = (int16_t)mixed(wr[i], s, g3);
        }
    } else {
        for (; ptr < end; ptr++) {
            uint32_t i = ptr ^ S;
            int32_t s = in[i];
            dl[i] = (int16_t)mixed(dl[i], s, g0);
            dr[i] = (int16_t)mixed(dr[i], s, g1);
        }
    }
}

void alist_envmix_exp(struct hle_t *hle, bool init, bool aux, uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl,
                      uint16_t dmem_wr, uint16_t dmemi, uint16_t count, int16_t dry, int16_t wet, const int16_t *vol,
                      const int16_t *target, const int32_t *rate, uint32_t address) {
    const int16_t *const in = (int16_t *)(hle->alist_buffer + dmemi);
    int16_t *const dl = (int16_t *)(hle->alist_buffer + dmem_dl);
    int16_t *const dr = (int16_t *)(hle->alist_buffer + dmem_dr);
    int16_t *const wl = (int16_t *)(hle->alist_buffer + dmem_wl);
    int16_t *const wr = (int16_t *)(hle->alist_buffer + dmem_wr);
    struct ramp ramps[2];
    int32_t exp_seq[2], exp_rates[2];
    short save_buffer[40];
    uint32_t ptr = 0;

    memcpy(save_buffer, hle->dram + address, sizeof save_buffer);
    if (init) {
        ramps[0].value = vol[0] << 16;
        ramps[1].value = vol[1] << 16;
        ramps[0].target = target[0] << 16;
        ramps[1].target = target[1] << 16;
        exp_rates[0] = rate[0];
        exp_rates[1] = rate[1];
        exp_seq[0] = vol[0] * rate[0];
        exp_seq[1] = vol[1] * rate[1];
    } else {
        wet = *(int16_t *)(save_buffer + 0);
        dry = *(int16_t *)(save_buffer + 2);
        ramps[0].target = *(int32_t *)(save_buffer + 4);
        ramps[1].target = *(int32_t *)(save_buffer + 6);
        exp_rates[0] = *(int32_t *)(save_buffer + 8);
        exp_rates[1] = *(int32_t *)(save_buffer + 10);
        exp_seq[0] = *(int32_t *)(save_buffer + 12);
        exp_seq[1] = *(int32_t *)(save_buffer + 14);
        ramps[0].value = *(int32_t *)(save_buffer + 16);
        ramps[1].value = *(int32_t *)(save_buffer + 18);
    }
    ramps[0].step = ramps[0].target - ramps[0].value;
    ramps[1].step = ramps[1].target - ramps[1].value;

    for (int y = 0; y < count; y += 16) {
        if (ramps[0].step == 0 && ramps[1].step == 0) {
            // A still ramp's first step snaps a value below target up to it, then nothing changes again
            for (int k = 0; k < 2; k++)
                if (ramps[k].value <= ramps[k].target) ramps[k].value = ramps[k].target;
            int32_t l = (int16_t)(ramps[0].value >> 16), r = (int16_t)(ramps[1].value >> 16);
            envmix_still(dl, dr, wl, wr, in, aux, ptr, 8u * (uint32_t)((count - y + 15) / 16),
                         sat_i16((l * dry + 0x4000) >> 15), sat_i16((r * dry + 0x4000) >> 15),
                         sat_i16((l * wet + 0x4000) >> 15), sat_i16((r * wet + 0x4000) >> 15));
            break;
        }
        for (int k = 0; k < 2; k++)
            if (ramps[k].step != 0) {
                exp_seq[k] = ((int64_t)exp_seq[k] * (int64_t)exp_rates[k]) >> 16;
                ramps[k].step = (exp_seq[k] - ramps[k].value) >> 3;
            }
        for (int x = 0; x < 8; ++x, ++ptr) {
            int32_t l = ramp_step(&ramps[0]), r = ramp_step(&ramps[1]);
            uint32_t i = ptr ^ S;
            int32_t s = in[i];
            dl[i] = (int16_t)mixed(dl[i], s, sat_i16((l * dry + 0x4000) >> 15));
            dr[i] = (int16_t)mixed(dr[i], s, sat_i16((r * dry + 0x4000) >> 15));
            if (aux) {
                wl[i] = (int16_t)mixed(wl[i], s, sat_i16((l * wet + 0x4000) >> 15));
                wr[i] = (int16_t)mixed(wr[i], s, sat_i16((r * wet + 0x4000) >> 15));
            }
        }
    }

    *(int16_t *)(save_buffer + 0) = wet;
    *(int16_t *)(save_buffer + 2) = dry;
    *(int32_t *)(save_buffer + 4) = (int32_t)ramps[0].target;
    *(int32_t *)(save_buffer + 6) = (int32_t)ramps[1].target;
    *(int32_t *)(save_buffer + 8) = exp_rates[0];
    *(int32_t *)(save_buffer + 10) = exp_rates[1];
    *(int32_t *)(save_buffer + 12) = exp_seq[0];
    *(int32_t *)(save_buffer + 14) = exp_seq[1];
    *(int32_t *)(save_buffer + 16) = (int32_t)ramps[0].value;
    *(int32_t *)(save_buffer + 18) = (int32_t)ramps[1].value;
    memcpy(hle->dram + address, save_buffer, sizeof save_buffer);
}

static inline uint8_t dmem_byte(struct hle_t *hle, uint16_t dmem) { return hle->alist_buffer[(dmem ^ S8) & 0xfff]; }
static inline int16_t *dmem_s16(struct hle_t *hle, uint16_t dmem) {
    return (int16_t *)(hle->alist_buffer + ((dmem ^ S16) & 0xfff));
}

// adpcm_compute_residuals for 8 samples, unrolled; l1, l2 are the two samples before them
static inline void residuals8(int16_t *dst, const int16_t *src, const int16_t *b1, const int16_t *b2, int32_t l1,
                              int32_t l2) {
    int32_t s[8], c[8];
    for (int i = 0; i < 8; i++) s[i] = src[i], c[i] = b2[i];
#pragma GCC unroll 8
    for (int i = 0; i < 8; i++) {
        uint32_t a = (uint32_t)(s[i] * 2048) + (uint32_t)(b1[i] * l1) + (uint32_t)(c[i] * l2);
#pragma GCC unroll 8
        for (int k = 0; k < i; k++) a += (uint32_t)(c[k] * s[i - 1 - k]);
        dst[i] = (int16_t)sat_i16((int32_t)a >> 11);
    }
}

#define NIB_HI(b, sh) ((int32_t)(int16_t)(((b) & 0xf0) << 8) >> (sh))
#define NIB_LO(b, sh) ((int32_t)(int16_t)(((b) & 0x0f) << 12) >> (sh))
#define RES(n) ((int16_t)sat_i16((int32_t)(n) >> 11))

// A predictor's 8 residual rows back to back: row i is 2048, book2[0..i], book1[i] (i + 3 terms, 52 in all)
#define ROWS_LEN 52
static void build_rows(int16_t *m, const int16_t *cb) {
    for (int i = 0; i < 8; i++) {
        *m++ = 2048;
        for (int k = 0; k <= i; k++) *m++ = cb[8 + k];
        *m++ = cb[i];
    }
}

// Row i dotted with x[7 - i ..] = s_i .. s_0, l2, l1: mac.w's 64-bit sum has mupen's wrapped int32 in MACL
#ifdef __sh__
#define MW "mac.w @%0+,@%1+\n\t"
#define ROW(i, seq)                                                                                           \
    do {                                                                                                      \
        const int16_t *xp = x + 7 - (i);                                                                      \
        int32_t r;                                                                                            \
        __asm__ volatile("clrmac\n\t" seq "sts macl,%2" : "+r"(xp), "+r"(m), "=r"(r) : : "mach", "macl", "memory"); \
        o[i] = RES(r);                                                                                        \
    } while (0)
#else
#define MW
#define ROW(i, seq)                                                                       \
    do {                                                                                  \
        const int16_t *xp = x + 7 - (i);                                                  \
        uint32_t a = 0;                                                                   \
        for (int k = 0; k < (i) + 3; k++) a += (uint32_t)(m[k] * xp[k]);                  \
        m += (i) + 3;                                                                     \
        o[i] = RES((int32_t)a);                                                           \
    } while (0)
#endif

// residuals8 on 4 code bytes; o[] gets the 8 outputs
static inline void half_rows(int16_t *o, const uint8_t *buf, uint32_t at, unsigned sh, const int16_t *m,
                             int16_t l1, int16_t l2) {
    int16_t x[10] __attribute__((aligned(4)));
    uint32_t B0 = buf[(at + 0) ^ S8], B1 = buf[(at + 1) ^ S8], B2 = buf[(at + 2) ^ S8], B3 = buf[(at + 3) ^ S8];
    x[7] = (int16_t)NIB_HI(B0, sh), x[6] = (int16_t)NIB_LO(B0, sh), x[5] = (int16_t)NIB_HI(B1, sh);
    x[4] = (int16_t)NIB_LO(B1, sh), x[3] = (int16_t)NIB_HI(B2, sh), x[2] = (int16_t)NIB_LO(B2, sh);
    x[1] = (int16_t)NIB_HI(B3, sh), x[0] = (int16_t)NIB_LO(B3, sh), x[8] = l2, x[9] = l1;
    ROW(0, MW MW MW);
    ROW(1, MW MW MW MW);
    ROW(2, MW MW MW MW MW);
    ROW(3, MW MW MW MW MW MW);
    ROW(4, MW MW MW MW MW MW MW);
    ROW(5, MW MW MW MW MW MW MW MW);
    ROW(6, MW MW MW MW MW MW MW MW MW);
    ROW(7, MW MW MW MW MW MW MW MW MW MW);
}

static int16_t adpcm_rows[16][ROWS_LEN] __attribute__((aligned(4)));

// half_rows on bytes in plain order
static inline void half_rows_plain(int16_t *o, const uint8_t *p, unsigned sh, const int16_t *m, int16_t l1, int16_t l2) {
    int16_t x[10] __attribute__((aligned(4)));
    uint32_t B0 = p[0], B1 = p[1], B2 = p[2], B3 = p[3];
    x[7] = (int16_t)NIB_HI(B0, sh), x[6] = (int16_t)NIB_LO(B0, sh), x[5] = (int16_t)NIB_HI(B1, sh);
    x[4] = (int16_t)NIB_LO(B1, sh), x[3] = (int16_t)NIB_HI(B2, sh), x[2] = (int16_t)NIB_LO(B2, sh);
    x[1] = (int16_t)NIB_HI(B3, sh), x[0] = (int16_t)NIB_LO(B3, sh), x[8] = l2, x[9] = l1;
    ROW(0, MW MW MW);
    ROW(1, MW MW MW MW);
    ROW(2, MW MW MW MW MW);
    ROW(3, MW MW MW MW MW MW);
    ROW(4, MW MW MW MW MW MW MW);
    ROW(5, MW MW MW MW MW MW MW MW);
    ROW(6, MW MW MW MW MW MW MW MW MW);
    ROW(7, MW MW MW MW MW MW MW MW MW MW);
}

// The sample cache's decode (harness/aica_cache.c): n 4-bit frames of 9 plain-order bytes, each after last, each
// frame's 16 samples to out; bit for bit alist_adpcm's. book_id names the codebook, for the rows built from it
void adpcm_frames_fast(int16_t *last, const uint8_t *in, uint32_t n, const int16_t *codebook, uint32_t book_id, int16_t *out) {
    static int16_t rows[16][ROWS_LEN] __attribute__((aligned(4)));
    static uint32_t rows_book, built;
    if (rows_book != book_id) rows_book = book_id, built = 0;
#ifdef __sh__
    __asm__ volatile("clrs");
#endif
    for (; n; n--, in += 9, out += 16) {
        uint32_t e = in[0] & 0xf;
        unsigned scale = in[0] >> 4, sh = scale < 12 ? 12 - scale : 0;
        if (!(built >> e & 1)) {
            build_rows(rows[e], codebook + (e << 4));
            built |= 1u << e;
        }
        half_rows_plain(last, in + 1, sh, rows[e], last[14], last[15]);
        half_rows_plain(last + 8, in + 5, sh, rows[e], last[6], last[7]);
        for (int k = 0; k < 16; k++) out[k] = last[k];
    }
}

// 4-bit frames whose code bytes and outputs don't wrap DMEM, outputs 4-byte aligned: two samples per word store
static void adpcm4_fast(struct hle_t *hle, int16_t *last, uint16_t dmemo, uint16_t dmemi, uint16_t count,
                        const int16_t *codebook) {
    uint8_t *buf = hle->alist_buffer;
    uint32_t in = dmemi & 0xfff, built = 0;
    uint32_t *out = (uint32_t *)(buf + (dmemo & 0xfff));
#ifdef __sh__
    __asm__ volatile("clrs");  // MAC.W saturates when S is set
#endif
    for (; count; count -= 32, in += 9, out += 8) {
        uint32_t code = buf[in ^ S8], e = code & 0xf;
        unsigned scale = code >> 4, sh = scale < 12 ? 12 - scale : 0;
        if (!(built >> e & 1)) {
            build_rows(adpcm_rows[e], codebook + (e << 4));
            built |= 1u << e;
        }
        half_rows(last, buf, in + 1, sh, adpcm_rows[e], last[14], last[15]);
        half_rows(last + 8, buf, in + 5, sh, adpcm_rows[e], last[6], last[7]);
        for (int k = 0; k < 8; k++) out[k] = (uint32_t)(uint16_t)last[2 * k] << 16 | (uint16_t)last[2 * k + 1];
    }
}

void alist_adpcm(struct hle_t *hle, bool init, bool loop, bool two_bit_per_sample, uint16_t dmemo, uint16_t dmemi,
                 uint16_t count, const int16_t *codebook, uint32_t loop_address, uint32_t last_frame_address) {
    int16_t last_frame[16];
    if (init) memset(last_frame, 0, sizeof last_frame);
    else dram_load_u16(hle, (uint16_t *)last_frame, loop ? loop_address : last_frame_address, 16);
    for (int i = 0; i < 16; ++i, dmemo += 2) *dmem_s16(hle, dmemo) = last_frame[i];

    if (!two_bit_per_sample && !(dmemo & 3) && (dmemo & 0xfff) + count <= 0x1000u
        && (dmemi & 0xfff) + 9u * (count / 32u) <= 0x1000u) {
        adpcm4_fast(hle, last_frame, dmemo, dmemi, count, codebook);
        count = 0;
    }
    while (count != 0) {
        int16_t frame[16];
        uint8_t code = dmem_byte(hle, dmemi++);
        unsigned scale = code >> 4;
        const int16_t *const cb = codebook + ((code & 0xf) << 4);
        if (two_bit_per_sample) {
            unsigned rshift = scale < 14 ? 14 - scale : 0;
            for (int i = 0; i < 4; ++i) {
                uint8_t b = dmem_byte(hle, dmemi++);
                frame[4 * i + 0] = (int16_t)((b & 0xc0) << 8) >> rshift;
                frame[4 * i + 1] = (int16_t)((b & 0x30) << 10) >> rshift;
                frame[4 * i + 2] = (int16_t)((b & 0x0c) << 12) >> rshift;
                frame[4 * i + 3] = (int16_t)((b & 0x03) << 14) >> rshift;
            }
        } else {
            unsigned rshift = scale < 12 ? 12 - scale : 0;
            for (int i = 0; i < 8; ++i) {
                uint8_t b = dmem_byte(hle, dmemi++);
                frame[2 * i + 0] = (int16_t)((b & 0xf0) << 8) >> rshift;
                frame[2 * i + 1] = (int16_t)((b & 0x0f) << 12) >> rshift;
            }
        }
        residuals8(last_frame, frame, cb, cb + 8, last_frame[14], last_frame[15]);
        residuals8(last_frame + 8, frame + 8, cb, cb + 8, last_frame[6], last_frame[7]);
        for (int i = 0; i < 16; ++i, dmemo += 2) *dmem_s16(hle, dmemo) = last_frame[i];
        count -= 32;
    }
    dram_store_u16(hle, (uint16_t *)last_frame, last_frame_address, 16);
}

static inline int16_t *sample(struct hle_t *hle, unsigned pos) { return (int16_t *)hle->alist_buffer + ((pos ^ S) & 0xfff); }

// RESAMPLE_LUT rows as 6 taps over memory b[p & ~1 ..]: even p holds s1 s0 s3 s2, odd p holds s0 x s2 s1 x s3
static const int16_t (*resample_lut6(void))[6] {
    static int16_t t[128][6] __attribute__((aligned(4)));
    static int built;
    if (!built) {
        for (int r = 0; r < 64; r++) {
            const int16_t *l = RESAMPLE_LUT + 4 * r;
            int16_t *e = t[r], *o = t[64 + r];
            e[0] = l[1], e[1] = l[0], e[2] = l[3], e[3] = l[2], e[4] = 0, e[5] = 0;
            o[0] = l[0], o[1] = 0, o[2] = l[2], o[3] = l[1], o[4] = 0, o[5] = l[3];
        }
        built = 1;
    }
    return (const int16_t(*)[6])t;
}

static inline int32_t dot6(const int16_t *xp, const int16_t *m) {
#ifdef __sh__
    int32_t r;
    __asm__ volatile("clrmac\n\t" MW MW MW MW MW MW "sts macl,%2" : "+r"(xp), "+r"(m), "=r"(r) : : "mach", "macl", "memory");
    return r;
#else
    uint32_t a = 0;
    for (int k = 0; k < 6; k++) a += (uint32_t)(xp[k] * m[k]);
    return (int32_t)a;
#endif
}

static inline int32_t resample_one(int32_t s0, int32_t s1, int32_t s2, int32_t s3, uint32_t accu) {
    const int16_t *lut = RESAMPLE_LUT + ((accu & 0xfc00) >> 8);
    uint32_t a = (uint32_t)(s0 * lut[0]) + (uint32_t)(s1 * lut[1]) + (uint32_t)(s2 * lut[2]) + (uint32_t)(s3 * lut[3]);
    return sat_i16((int32_t)a >> 15);
}

void alist_resample(struct hle_t *hle, bool init, bool flag2, uint16_t dmemo, uint16_t dmemi, uint16_t count,
                    uint32_t pitch, uint32_t address) {
    uint32_t pitch_accu;
    uint16_t ipos = dmemi >> 1, opos = dmemo >> 1;
    count >>= 1;
    ipos -= 4;
    if (flag2) HleWarnMessage(hle->user_defined, "alist_resample: flag2 is not implemented");

    if (init) {
        for (unsigned k = 0; k < 4; ++k) *sample(hle, ipos + k) = 0;
        pitch_accu = 0;
    } else {
        for (unsigned k = 0; k < 4; ++k) *sample(hle, ipos + k) = *dram_u16(hle, address + 2 * k);
        pitch_accu = *dram_u16(hle, address + 8);
    }

    // Positions that don't wrap the mask: 6 taps from the even sample at or before ipos, swizzle folded into the LUT
    uint32_t is = ipos & 0xfff, os = opos & 0xfff;
    uint32_t ie = is + (uint32_t)(((uint64_t)pitch_accu + (uint64_t)pitch * count) >> 16), oe = os + count;
    if (count && ie + 6 <= 0x1000 && oe <= 0x1000) {
        int16_t *b = (int16_t *)hle->alist_buffer;
        const int16_t(*lut)[6] = resample_lut6();
        uint32_t cur = is;
#ifdef __sh__
        __asm__ volatile("clrs");
#endif
        for (uint32_t o = os; o < oe; o++) {
            const int16_t *xp = b + (cur & ~1u), *m = lut[(cur & 1) << 6 | (pitch_accu & 0xfc00) >> 10];
            b[o ^ S] = (int16_t)sat_i16(dot6(xp, m) >> 15);
            pitch_accu += pitch;
            cur += pitch_accu >> 16;
            pitch_accu &= 0xffff;
        }
        ipos = (uint16_t)(ipos + (cur - is));
    } else {
        while (count != 0) {
            *sample(hle, opos++) = (int16_t)resample_one(*sample(hle, ipos), *sample(hle, ipos + 1),
                                                         *sample(hle, ipos + 2), *sample(hle, ipos + 3), pitch_accu);
            pitch_accu += pitch;
            ipos += pitch_accu >> 16;
            pitch_accu &= 0xffff;
            --count;
        }
    }

    for (unsigned k = 0; k < 4; ++k) *dram_u16(hle, address + 2 * k) = *sample(hle, ipos + k);
    *dram_u16(hle, address + 8) = pitch_accu;
}

// mupen's loop, plus: with the wet gain 0 for the whole call and no wet xors, wl/wr get clamp(w + 0) = w, so skip them
void alist_envmix_nead(struct hle_t *hle, bool swap_wet_LR, uint16_t dmem_dl, uint16_t dmem_dr, uint16_t dmem_wl,
                       uint16_t dmem_wr, uint16_t dmemi, unsigned count, uint16_t *env_values, uint16_t *env_steps,
                       const int16_t *xors) {
    int16_t *in = (int16_t *)(hle->alist_buffer + dmemi);
    int16_t *dl = (int16_t *)(hle->alist_buffer + dmem_dl);
    int16_t *dr = (int16_t *)(hle->alist_buffer + dmem_dr);
    int16_t *wl = (int16_t *)(hle->alist_buffer + dmem_wl);
    int16_t *wr = (int16_t *)(hle->alist_buffer + dmem_wr);
    count = (count + 7) & ~7u;
    if (swap_wet_LR) {
        int16_t *t = wl;
        wl = wr, wr = t;
    }
    if (env_values[2] == 0 && env_steps[2] == 0 && xors[2] == 0 && xors[3] == 0) {
        for (; count != 0; count -= 8, dl += 8, dr += 8, in += 8) {
            for (size_t i = 0; i < 8; ++i) {
                int16_t l = (((int32_t)in[i ^ S] * (uint32_t)env_values[0]) >> 16) ^ xors[0];
                int16_t r = (((int32_t)in[i ^ S] * (uint32_t)env_values[1]) >> 16) ^ xors[1];
                dl[i ^ S] = clamp_s16(dl[i ^ S] + l);
                dr[i ^ S] = clamp_s16(dr[i ^ S] + r);
            }
            env_values[0] += env_steps[0];
            env_values[1] += env_steps[1];
        }
        return;
    }
    for (; count != 0; count -= 8, dl += 8, dr += 8, wl += 8, wr += 8, in += 8) {
        for (size_t i = 0; i < 8; ++i) {
            int16_t l = (((int32_t)in[i ^ S] * (uint32_t)env_values[0]) >> 16) ^ xors[0];
            int16_t r = (((int32_t)in[i ^ S] * (uint32_t)env_values[1]) >> 16) ^ xors[1];
            int16_t l2 = (((int32_t)l * (uint32_t)env_values[2]) >> 16) ^ xors[2];
            int16_t r2 = (((int32_t)r * (uint32_t)env_values[2]) >> 16) ^ xors[3];
            dl[i ^ S] = clamp_s16(dl[i ^ S] + l);
            dr[i ^ S] = clamp_s16(dr[i ^ S] + r);
            wl[i ^ S] = clamp_s16(wl[i ^ S] + l2);
            wr[i ^ S] = clamp_s16(wr[i ^ S] + r2);
        }
        env_values[0] += env_steps[0];
        env_values[1] += env_steps[1];
        env_values[2] += env_steps[2];
    }
}

// Whole 4-byte groups are the same bytes swizzled or not, so they clear with memset
void alist_clear(struct hle_t *hle, uint16_t dmem, uint16_t count) {
    for (; count && (dmem & 3); count--, dmem++) hle->alist_buffer[(dmem ^ S8) & 0xfff] = 0;
    while (count >= 4) {
        uint32_t at = dmem & 0xfff, n = count & ~3u;
        if (n > 0x1000 - at) n = 0x1000 - at;
        memset(hle->alist_buffer + at, 0, n);
        dmem = (uint16_t)(dmem + n), count = (uint16_t)(count - n);
    }
    for (; count; count--, dmem++) hle->alist_buffer[(dmem ^ S8) & 0xfff] = 0;
}
