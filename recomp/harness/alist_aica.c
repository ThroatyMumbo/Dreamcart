// Audio command lists read as voices: only the commands that feed each voice's final resampler run; the AICA
// resamples, mixes and pans. ABI1 (SM64), nead (MK64) and naudio (Kirby 64), on mupen64plus-rsp-hle's primitives.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "alist.h"
#include "hle_internal.h"
#include "memory.h"

#include "aica_voice.h"
#include "alist_aica.h"
#include "alist_decode.h"
#include "os.h"
#include "pi_ledger.h"
#include "rcp.h"
#include "rsp_audio.h"

#define NGR 512u  // 8-byte DMEM granules
#define MAXV 96u

static uint32_t live[NGR / 32];
static uint16_t livew;  // the nonzero words of live
static int16_t cap_buf[4096];
static struct av_src srcs[MAXV];

struct aa_stats aa_stats;

static uint32_t al(uint32_t v, uint32_t n) { return (v + n - 1) & ~(n - 1); }
static int16_t *smp(struct hle_t *h, uint32_t pos) { return (int16_t *)h->alist_buffer + ((pos ^ S) & 0xfff); }

// Granules [g0, g1) of the liveness bitmap, a word at a time: 0 tests, 1 sets, 2 clears
static int span(uint32_t g0, uint32_t g1, int op) {
    while (g0 < g1) {
        uint32_t b = g0 & 31, k = g1 - g0 < 32 - b ? g1 - g0 : 32 - b;
        uint32_t m = (k == 32 ? ~0u : (1u << k) - 1) << b, *w = &live[g0 >> 5];
        if (op == 0 && *w & m) return 1;
        if (op == 1) *w |= m, livew |= (uint16_t)(1u << (g0 >> 5));
        if (op == 2 && !(*w &= ~m)) livew &= (uint16_t)~(1u << (g0 >> 5));
        g0 += k;
    }
    return 0;
}

static int any_live(const struct rng *r) { return span(r->a >> 3, (r->a + r->n + 7u) >> 3, 0); }
static void mark(const struct rng *r) { span(r->a >> 3, (r->a + r->n + 7u) >> 3, 1); }
// Only granules the write covers completely stop being live
static void kill(const struct rng *r) { span((r->a + 7u) >> 3, (uint32_t)(r->a + r->n) >> 3, 2); }

static int writes(const struct cmd *c, uint32_t a) {
    for (uint32_t k = 0; k < c->nw; k++)
        if (a - CW(c, k).a < CW(c, k).n) return 1;
    return 0;
}

static uint16_t vres[MAXC];  // the voice resamplers, in list order
static uint32_t nvres;

// Each envelope mixer's input traced to the command that last wrote it; a resampler there is a voice
__attribute__((noinline)) static void find_voices(uint32_t n) {
    nvres = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct cmd *c = &cmds[i];
        if (!(c->flags & (F_ENV | F_MIX))) continue;
        uint32_t a = c->in & 0xFFF, j = i;
        while (j-- > 0 && !writes(&cmds[j], a)) {}
        int fed = j < i && (cmds[j].flags & F_RES) && cmds[j].out == c->in;
        if (!fed && (c->flags & F_MIX)) {
            c->flags &= (uint16_t)~F_MIX;  // a plain mix: reverb returns, buses
            continue;
        }
        if (fed) {
            aa_stats.mixed += (c->flags & F_MIX) != 0;
            if (!(cmds[j].flags & F_CAP)) {
                uint32_t k = nvres++;
                for (; k > 0 && vres[k - 1] > j; k--) vres[k] = vres[k - 1];
                vres[k] = (uint16_t)j;
            }
            cmds[j].flags |= F_CAP;
            c->feed = (uint16_t)(j + 1);
        } else {
            c->flags |= F_CAP;  // the stream reaches the mixer unresampled
            aa_stats.unattributed++;
        }
    }
}

// Backward liveness from the captured streams: whatever writes nothing live (mixers, reverb, the AI buffer) is dead
__attribute__((noinline)) static void eliminate(uint32_t n) {
    int table_live = 0, book_later = 0;
    memset(live, 0, sizeof live);
    livew = 0;
    for (uint32_t i = n; i-- > 0;) {
        struct cmd *c = &cmds[i];
        if (c->flags & F_STATE) {
            if (!(c->flags & F_TABW)) c->flags |= F_KEEP;
            else {  // a codebook nothing live reads before the next one replaces it
                if (table_live || !book_later) c->flags |= F_KEEP;
                table_live = 0, book_later = 1;
            }
            continue;
        }
        int need = (c->flags & F_TABW) && table_live;
        if (livew & c->wm)
            for (uint32_t k = 0; k < c->nw && !need; k++) need = any_live(&CW(c, k));
        if ((c->flags & (F_CAP | F_RES)) == (F_CAP | F_RES) && !need) {
            kill(&CW(c, 0));  // the history the skipped resampler still loads
            if (!(c->flags & F_RESID)) mark(&(struct rng){ c->in & 0xFFFu, (uint16_t)(CR(c, 0).n - 8) });
            continue;
        }
        if ((c->flags & (F_CAP | F_ENV)) == (F_CAP | F_ENV)) mark(&CR(c, 0));
        if (!need) continue;
        c->flags |= (c->flags & F_CAP) && (c->flags & F_RES) ? F_EXEC : F_KEEP;
        for (uint32_t k = 0; k < c->nw; k++) kill(&CW(c, k));
        for (uint32_t k = 0; k < c->nr; k++) mark(&CR(c, k));
        if (c->flags & F_TABR) table_live = 1;
    }
}

static void capture(struct hle_t *h, uint32_t id, int init, uint32_t in, uint32_t n, uint32_t cnt, uint32_t pitch,
                    struct av_src *vsrc) {
    if (n > sizeof cap_buf / 2) os_die("alist_aica: capture longer than its buffer", n);
    uint32_t p = in >> 1, k = 0;
    if (p & 1) cap_buf[k++] = *smp(h, p++);
    // mupen's S swaps the two samples of each word, so an even-aligned run copies a word at a time
    const uint32_t *src = (const uint32_t *)h->alist_buffer;
    uint32_t *dst = (uint32_t *)cap_buf;
    if (!(k & 1) && ((p + n - k) & 0xFFF) > (p & 0xFFF))
        for (; k + 1 < n; k += 2, p += 2) { uint32_t w = src[(p & 0xFFF) >> 1]; dst[k >> 1] = w << 16 | w >> 16; }
    for (; k < n; k++) cap_buf[k] = *smp(h, p++);
    av_voice(id, init, cap_buf, n, cnt, pitch, vsrc);
}

// mupen's alist_resample without the filter: the same history and position, so its RDRAM state is unchanged
static void resample_state(struct hle_t *h, int init, uint32_t in, uint32_t cnt, uint32_t pitch, uint32_t addr) {
    uint32_t ipos = ((in >> 1) - 4) & 0xFFFF, accu;
    if (init) { for (int k = 0; k < 4; k++) *smp(h, ipos + k) = 0; accu = 0; }
    else { for (int k = 0; k < 4; k++) *smp(h, ipos + k) = (int16_t)*dram_u16(h, addr + 2 * k); accu = *dram_u16(h, addr + 8); }
    accu += (cnt >> 1) * pitch;
    ipos = (ipos + (accu >> 16)) & 0xFFFF;
    for (int k = 0; k < 4; k++) *dram_u16(h, addr + 2 * k) = (uint16_t)*smp(h, ipos + k);
    *dram_u16(h, addr + 8) = (uint16_t)accu;
}

static uint32_t consumed(struct hle_t *h, int init, uint32_t cnt, uint32_t pitch, uint32_t addr) {
    uint32_t accu = init ? 0 : *dram_u16(h, addr + 8);
    return (accu + (cnt >> 1) * pitch) >> 16;
}

// ABI1's envelope reduced to its target: the state mupen saves, with the ramp already at its end
static void envmix_abi1(struct hle_t *h, uint32_t id, int init, uint32_t addr) {
    struct alist_audio_t *s = &h->alist_audio;
    uint32_t sb[20];
    int16_t *sh = (int16_t *)sb, wet, dry;
    int32_t vl, vr;
    if (init) {
        memset(sb, 0, sizeof sb);
        wet = s->wet, dry = s->dry;
        vl = s->target[0] << 16, vr = s->target[1] << 16;
        sh[0] = wet, sh[2] = dry;
        sb[2] = (uint32_t)vl, sb[3] = (uint32_t)vr, sb[4] = (uint32_t)s->rate[0], sb[5] = (uint32_t)s->rate[1];
        sb[6] = (uint32_t)(s->target[0] * s->rate[0]), sb[7] = (uint32_t)(s->target[1] * s->rate[1]);
        sb[8] = (uint32_t)vl, sb[9] = (uint32_t)vr;
        memcpy(h->dram + addr, sb, sizeof sb);
    } else {
        memcpy(sb, h->dram + addr, sizeof sb);
        wet = sh[0], dry = sh[2];
        vl = (int32_t)sb[8], vr = (int32_t)sb[9];
    }
    int32_t l = (int16_t)(vl >> 16), r = (int16_t)(vr >> 16);
    int32_t gl = sat_i16((l * dry + 0x4000) >> 15), gr = sat_i16((r * dry + 0x4000) >> 15);
    int32_t gw = sat_i16(((l > r ? l : r) * wet + 0x4000) >> 15);
    av_gain(id, gl < 0 ? -gl : gl, gr < 0 ? -gr : gr, gw < 0 ? -gw : gw);
}

// nead's envelope: its values step once per 8 samples; the gains are where they end
static void envmix_nead(struct hle_t *h, uint32_t id, uint32_t samples) {
    struct alist_nead_t *s = &h->alist_nead;
    for (uint32_t k = 0; k < samples; k += 8)
        for (int j = 0; j < 3; j++) s->env_values[j] += s->env_steps[j];
    uint32_t l = s->env_values[0] >> 1, r = s->env_values[1] >> 1;
    av_gain(id, l, r, ((l > r ? l : r) * s->env_values[2]) >> 16);
}

// k of alist_envmix_lin's ramp_step at once: the ramp is monotone, so it has reached its target iff its end has
static void ramp_steps(int64_t *v, int64_t *step, int64_t target, uint32_t k) {
    if (!k) return;
    *v += (int64_t)k * *step;
    if (*step <= 0 ? *v <= target : *v >= target) *v = target, *step = 0;
}

// naudio's linear envelope over `samples` (mupen's save buffer left as alist_envmix_lin leaves it); the gains are
// where the update ends, since its ramps span many updates
static void envmix_naudio(struct hle_t *h, uint32_t id, int init, uint32_t addr, uint32_t w1, uint32_t samples) {
    struct alist_naudio_t *s = &h->alist_naudio;
    uint32_t sb[20];
    int16_t *sh = (int16_t *)sb, wet, dry;
    int64_t v[2], st[2], t[2];
    s->vol[1] = (int16_t)w1;
    memcpy(sb, h->dram + addr, sizeof sb);
    if (init) {
        wet = s->wet, dry = s->dry;
        for (int j = 0; j < 2; j++) st[j] = s->rate[j] / 8, v[j] = (int32_t)s->vol[j] << 16, t[j] = (int32_t)s->target[j] << 16;
    } else {
        wet = sh[0], dry = sh[2];
        t[0] = (int32_t)sh[4] << 16, t[1] = (int32_t)sh[6] << 16;
        st[0] = (int32_t)sb[4], st[1] = (int32_t)sb[5];
        v[0] = (int32_t)sb[8], v[1] = (int32_t)sb[9];
    }
    for (int j = 0; j < 2; j++) ramp_steps(&v[j], &st[j], t[j], samples);
    if (samples) {
        sh[0] = wet, sh[2] = dry;
        sh[4] = (int16_t)(t[0] >> 16), sh[6] = (int16_t)(t[1] >> 16);
        sb[4] = (uint32_t)(int32_t)st[0], sb[5] = (uint32_t)(int32_t)st[1];
        sb[8] = (uint32_t)(int32_t)v[0], sb[9] = (uint32_t)(int32_t)v[1];
        memcpy(h->dram + addr, sb, sizeof sb);
    }
    int32_t l = (int16_t)(v[0] >> 16), r = (int16_t)(v[1] >> 16);
    int32_t gl = sat_i16((l * dry + 0x4000) >> 15), gr = sat_i16((r * dry + 0x4000) >> 15);
    int32_t gw = sat_i16(((l > r ? l : r) * wet + 0x4000) >> 15);
    av_gain(id, gl < 0 ? -gl : gl, gr < 0 ? -gr : gr, gw < 0 ? -gw : gw);
}

static void exec_abi1(struct hle_t *h, uint32_t w1, uint32_t w2) {
    struct alist_audio_t *s = &h->alist_audio;
    uint32_t flags = (w1 >> 16) & 0xFF, addr = alist_get_address(h, w2, s->segments, N_SEGMENTS);
    switch ((w1 >> 24) & 0x7F) {
    case 1:
        alist_adpcm(h, flags & A_INIT, flags & A_LOOP, false, s->out, s->in, al(s->count, 32), s->table, s->loop, addr);
        break;
    case 2: if (w2 & 0xFFF) alist_clear(h, (uint16_t)(w1 + ABI1_DMEM_BASE), (uint16_t)al(w2 & 0xFFF, 16)); break;
    case 3:
        alist_envmix_exp(h, flags & A_INIT, flags & A_AUX, s->out, s->dry_right, s->wet_left, s->wet_right, s->in,
                         s->count, s->dry, s->wet, s->vol, s->target, s->rate, addr);
        break;
    case 4: if (s->count) alist_load(h, s->in, addr, s->count); break;
    case 5:
        alist_resample(h, flags & A_INIT, flags & 2, s->out, s->in, (uint16_t)al(s->count, 16), (w1 & 0xFFFF) << 1, addr);
        break;
    case 6: if (s->count) alist_save(h, s->out, addr, s->count); break;
    case 7: alist_set_address(h, w2, s->segments, N_SEGMENTS); break;
    case 8:
        if (flags & A_AUX) { s->dry_right = (uint16_t)(w1 + ABI1_DMEM_BASE); s->wet_left = (uint16_t)((w2 >> 16) + ABI1_DMEM_BASE); s->wet_right = (uint16_t)(w2 + ABI1_DMEM_BASE); }
        else { s->in = (uint16_t)(w1 + ABI1_DMEM_BASE); s->out = (uint16_t)((w2 >> 16) + ABI1_DMEM_BASE); s->count = (uint16_t)w2; }
        break;
    case 9:
        if (flags & A_AUX) { s->dry = (int16_t)w1; s->wet = (int16_t)w2; }
        else if (flags & A_VOL) s->vol[(flags & A_LEFT) ? 0 : 1] = (int16_t)w1;
        else { s->target[(flags & A_LEFT) ? 0 : 1] = (int16_t)w1; s->rate[(flags & A_LEFT) ? 0 : 1] = (int32_t)w2; }
        break;
    case 10: if (w2 & 0xFFFF) alist_move(h, (uint16_t)((w2 >> 16) + ABI1_DMEM_BASE), (uint16_t)(w1 + ABI1_DMEM_BASE), (uint16_t)al(w2 & 0xFFFF, 16)); break;
    case 11: dram_load_u16(h, (uint16_t *)s->table, addr, al(w1 & 0xFFFF, 8) >> 1); break;
    case 12:
        if (s->count) alist_mix(h, (uint16_t)(w2 + ABI1_DMEM_BASE), (uint16_t)((w2 >> 16) + ABI1_DMEM_BASE), (uint16_t)al(s->count, 32), (int16_t)w1);
        break;
    case 13:
        if (s->count) alist_interleave(h, s->out, (uint16_t)((w2 >> 16) + ABI1_DMEM_BASE), (uint16_t)(w2 + ABI1_DMEM_BASE), (uint16_t)al(s->count, 16));
        break;
    case 14:
        if (s->count) alist_polef(h, flags & A_INIT, s->out, s->in, (uint16_t)al(s->count, 16), (uint16_t)w1, s->table, addr);
        break;
    case 15: s->loop = addr; break;
    }
}

static void exec_nead(struct hle_t *h, uint32_t w1, uint32_t w2) {
    struct alist_nead_t *s = &h->alist_nead;
    uint32_t flags = (w1 >> 16) & 0xFF, addr = w2 & 0xFFFFFF;
    switch ((w1 >> 24) & 0x7F) {
    case 0x01:
        alist_adpcm(h, flags & 1, flags & 2, flags & 4, s->out, s->in, (uint16_t)((s->count + 0x1F) & ~0x1F), s->table, s->loop, addr);
        break;
    case 0x02: if (w2 & 0xFFF) alist_clear(h, (uint16_t)w1, (uint16_t)(w2 & 0xFFF)); break;
    case 0x05:
        alist_resample(h, flags & 1, false, s->out, s->in, (uint16_t)((s->count + 0xF) & ~0xF), (w1 & 0xFFFF) << 1, addr);
        break;
    case 0x08: s->in = (uint16_t)w1; s->out = (uint16_t)(w2 >> 16); s->count = (uint16_t)w2; break;
    case 0x0A: if (w2 & 0xFFFF) alist_move(h, (uint16_t)(w2 >> 16), (uint16_t)w1, (uint16_t)(((w2 & 0xFFFF) + 3) & ~3u)); break;
    case 0x0B: dram_load_u16(h, (uint16_t *)s->table, addr, (w1 & 0xFFFF) >> 1); break;
    case 0x0C: alist_mix(h, (uint16_t)w2, (uint16_t)(w2 >> 16), (uint16_t)((w1 >> 12) & 0xFF0), (int16_t)w1); break;
    case 0x0D: if (s->count) alist_interleave(h, s->out, (uint16_t)(w2 >> 16), (uint16_t)w2, s->count); break;
    case 0x0E:
        if (s->count) alist_polef(h, flags & A_INIT, s->out, s->in, s->count, (uint16_t)w1, s->table, addr);
        break;
    case 0x0F: s->loop = addr; break;
    case 0x10: alist_copy_blocks(h, (uint16_t)(w2 >> 16), (uint16_t)w1, (uint16_t)w2, (uint8_t)(w1 >> 16)); break;
    case 0x11: alist_copy_every_other_sample(h, (uint16_t)w2, (uint16_t)(w2 >> 16), (uint16_t)w1); break;
    case 0x12:
        s->env_values[2] = (w1 >> 8) & 0xFF00; s->env_steps[2] = 0;
        s->env_steps[0] = (uint16_t)(w2 >> 16); s->env_steps[1] = (uint16_t)w2;
        break;
    case 0x13: {
        int16_t xors[4] = { (int16_t)(0 - (int16_t)((w1 & 2) >> 1)), (int16_t)(0 - (int16_t)(w1 & 1)), 0, 0 };
        alist_envmix_nead(h, false, (uint16_t)((w2 >> 20) & 0xFF0), (uint16_t)((w2 >> 12) & 0xFF0), (uint16_t)((w2 >> 4) & 0xFF0),
                          (uint16_t)((w2 << 4) & 0xFF0), (uint16_t)((w1 >> 12) & 0xFF0), (w1 >> 8) & 0xFF, s->env_values, s->env_steps, xors);
        break;
    }
    case 0x14: alist_load(h, (uint16_t)(w1 & 0xFFF), addr, (uint16_t)((w1 >> 12) & 0xFFF)); break;
    case 0x15:
        alist_save(h, (uint16_t)(w1 & 0xFFF), addr, (uint16_t)((w1 >> 12) & 0xFFF));
        break;
    case 0x16: s->env_values[0] = (uint16_t)(w2 >> 16); s->env_values[1] = (uint16_t)w2; break;
    }
}

// mupen's alist_process_naudio, one command at a time
static void exec_naudio(struct hle_t *h, uint32_t w1, uint32_t w2) {
    struct alist_naudio_t *s = &h->alist_naudio;
    uint32_t flags = (w1 >> 16) & 0xFF, a1 = w1 & 0xFFFFFF, a2 = w2 & 0xFFFFFF;
    switch ((w1 >> 24) & 0x7F) {
    case 0x01: {
        uint32_t f = w2 >> 28;
        alist_adpcm(h, f & A_INIT, f & A_LOOP, false, (uint16_t)((w2 & 0xFFF) + NA_MAIN), (uint16_t)(((w2 >> 12) & 0xF) + NA_MAIN),
                    (uint16_t)((((w2 >> 16) & 0xFFF) + 0x1F) & ~0x1Fu), s->table, s->loop, a1);
        break;
    }
    case 0x02: alist_clear(h, (uint16_t)(w1 + NA_MAIN), (uint16_t)(w2 & 0xFFF)); break;
    case 0x03:
        s->vol[1] = (int16_t)w1;
        alist_envmix_lin(h, flags & A_INIT, NA_DL, NA_DR, NA_WL, NA_WR, NA_MAIN, NA_COUNT, s->dry, s->wet, s->vol, s->target,
                         s->rate, a2);
        break;
    case 0x04: alist_load(h, (uint16_t)((w1 & 0xFFF) + NA_MAIN), a2, (uint16_t)((w1 >> 12) & 0xFFF)); break;
    case 0x05:
        alist_resample(h, (w2 >> 30) & A_INIT, false, (uint16_t)(w2 & 3 ? NA_MAIN2 : NA_MAIN), (uint16_t)(((w2 >> 2) & 0xFFF) + NA_MAIN),
                       NA_COUNT, ((w2 >> 14) & 0xFFFF) << 1, a1);
        break;
    case 0x06:
        alist_save(h, (uint16_t)((w1 & 0xFFF) + NA_MAIN), a2, (uint16_t)((w1 >> 12) & 0xFFF));
        break;
    case 0x09:
        if (flags & A_VOL) {
            if (flags & A_LEFT) { s->vol[0] = (int16_t)w1; s->dry = (int16_t)(w2 >> 16); s->wet = (int16_t)w2; }
            else { s->target[1] = (int16_t)w1; s->rate[1] = (int32_t)w2; }
        } else { s->target[0] = (int16_t)w1; s->rate[0] = (int32_t)w2; }
        break;
    case 0x0A:
        alist_move(h, (uint16_t)((w2 >> 16) + NA_MAIN), (uint16_t)(w1 + NA_MAIN), (uint16_t)(((w2 & 0xFFFF) + 3) & ~3u));
        break;
    case 0x0B: dram_load_u16(h, (uint16_t *)s->table, a2, (w1 & 0xFFFF) >> 1); break;
    case 0x0C: alist_mix(h, (uint16_t)(w2 + NA_MAIN), (uint16_t)((w2 >> 16) + NA_MAIN), NA_COUNT, (int16_t)w1); break;
    case 0x0D: alist_interleave(h, NA_MAIN, NA_DL, NA_DR, NA_COUNT); break;
    case 0x0E: s->rate[1] = (int32_t)(((uint32_t)s->rate[1] & ~0xFFFFu) | (w2 & 0xFFFF)); break;
    case 0x0F: s->loop = a2; break;
    }
}

static void exec(struct hle_t *h, int abi, uint32_t w1, uint32_t w2) {
    if (abi == AA_NEAD) exec_nead(h, w1, w2);
    else if (abi == AA_NAUDIO) exec_naudio(h, w1, w2);
    else exec_abi1(h, w1, w2);
}

// A resampler's state address, which is its voice id
static uint32_t res_addr(struct hle_t *h, int abi, uint32_t w1, uint32_t w2) {
    if (abi == AA_NEAD) return w2 & 0xFFFFFF;
    if (abi == AA_NAUDIO) return w1 & 0xFFFFFF;
    return alist_get_address(h, w2, h->alist_audio.segments, N_SEGMENTS);
}

// A voice resampler's id, A_INIT and pitch from its command words; seg is ABI1's segment table at that command
static void voice_args(int abi, const uint32_t *seg, uint32_t w1, uint32_t w2, uint32_t *id, uint32_t *init, uint32_t *pitch) {
    *init = (w1 >> 16) & A_INIT;
    *pitch = (w1 & 0xFFFF) << 1;
    if (abi == AA_NEAD) *id = w2 & 0xFFFFFF;
    else if (abi == AA_NAUDIO) *id = w1 & 0xFFFFFF, *init = (w2 >> 30) & A_INIT, *pitch = ((w2 >> 14) & 0xFFFF) << 1;
    else *id = (((w2 >> 24) & 0x3F) < N_SEGMENTS ? seg[(w2 >> 24) & 0x3F] : 0) + (w2 & 0xFFFFFF);
}

#define NONE (~0u)
// first_at, init_at, wrap: the stream's first sample from any / the A_INIT / an A_LOOP ADPCM, at output byte first_x /
// init_x / loop_x of command first / init / loop; ad: every ADPCM command the stream comes from
struct prov {
    uint32_t first_at, first_x, init_at, init_x, wrap, loop_x, end;
    int first, init, loop;
    int16_t ad[AV_ADPCMS];
    struct { uint16_t at, n, j; int16_t cmd; } seg[AV_SEGS];
    uint8_t nad, nseg, foreign;
};

static void add_seg(struct prov *p, uint32_t at, uint32_t bytes, int cmd, uint32_t j) {
    if (p->nseg == AV_SEGS) { p->foreign = AA_FG_SEGS; return; }
    p->seg[p->nseg].at = (uint16_t)at, p->seg[p->nseg].n = (uint16_t)(bytes >> 1);
    p->seg[p->nseg].j = (uint16_t)j, p->seg[p->nseg++].cmd = (int16_t)cmd;
}

// What wrote DMEM [lo, hi) before command j, so the stream byte offset of lo: ADPCM output or a CLEAR, directly or
// through MOVEs; anything else makes the stream foreign
static void resolve(uint32_t j, uint32_t lo, uint32_t hi, uint32_t so, int depth, struct prov *p) {
    for (uint32_t i = j; i-- > 0;) {
        const struct cmd *c = &cmds[i];
        for (uint32_t k = 0; k < c->nw; k++) {
            uint32_t a = CW(c, k).a, b = a + CW(c, k).n;
            if (b <= lo || a >= hi) continue;
            uint32_t x = a > lo ? a : lo, y = b < hi ? b : hi, at = (so + (x - lo)) >> 1;
            if (lo < x) resolve(i + 1, lo, x, so, depth, p);
            if (y < hi) resolve(i + 1, y, hi, so + (y - lo), depth, p);
            if (c->kind == K_ADPCM) {
                uint32_t q = 0;
                while (q < p->nad && p->ad[q] != (int16_t)i) q++;
                if (q == p->nad) { if (q < AV_ADPCMS) p->ad[p->nad++] = (int16_t)i; else p->foreign = AA_FG_ADPCMS; }
                if (at < p->first_at) p->first_at = at, p->first_x = x, p->first = (int)i;
                if (c->af & A_INIT) { if (at < p->init_at) p->init_at = at, p->init_x = x, p->init = (int)i; }
                else if ((c->af & A_LOOP) && at < p->wrap) p->wrap = at, p->loop_x = x, p->loop = (int)i;
                add_seg(p, at, y - x, (int)i, (x - CW(c, 0).a) >> 1);
            } else if (c->kind == K_CLEAR) {
                if (at < p->end) p->end = at;
                add_seg(p, at, y - x, -1, 0);
            } else if (c->kind == K_MOVE && c->nr && depth < 4) {
                uint32_t s = CR(c, 0).a + (x - a);
                resolve(i, s, s + (y - x), so + (x - lo), depth + 1, p);
            } else p->foreign = c->kind == K_MOVE ? AA_FG_DEPTH : c->kind == K_LOAD ? AA_FG_LOAD : AA_FG_OTHER;
            return;
        }
    }
    p->foreign = AA_FG_NONE;
}

static uint64_t fnv8(uint64_t k, uint32_t b) { return (k ^ (b & 0xFF)) * 0x100000001B3ull; }

// The RDRAM address ADPCM command a's first input frame was loaded from, or NONE
static uint32_t frame_ram(uint32_t a) {
    const struct cmd *c = &cmds[a];
    if (!c->nr) return NONE;
    for (uint32_t i = a, in = CR(c, 0).a; i-- > 0;)
        if (writes(&cmds[i], in)) return cmds[i].kind == K_LOAD ? cmds[i].ram + (in - CW(&cmds[i], 0).a) : NONE;
    return NONE;
}

static struct hle_t *key_h;

// A ROM offset where the PI put the frame's bytes and the ROM still says so (the CPU may have rewritten them since),
// else the RDRAM address itself
extern uint32_t aa_ledger_calls;
static uint32_t frame_at(uint32_t ram, int *rom) {
    aa_ledger_calls++;
    uint32_t r = ram != NONE ? pi_ledger_rom(ram) : NONE;
    uint8_t b[9];
    if (r != NONE && (ram + 9 > 0x800000u || !aa_frame_src(1, r, b, 9))) r = NONE;
    for (uint32_t k = 0; r != NONE && k < 9; k++)
        if (b[k] != key_h->dram[(ram + k) ^ S8]) r = NONE;
    *rom = r != NONE;
    return r != NONE ? r : ram;
}

void aa_bytes(uint32_t ram, uint8_t *out, uint32_t n) {
    for (uint32_t k = 0; k < n; k++) out[k] = key_h->dram[((ram + k) ^ S8) & 0x7FFFFF];
}

void aa_book(uint32_t addr, uint32_t len, int16_t *out) { dram_load_u16(key_h, (uint16_t *)out, addr, len >> 1); }

int aa_frame_src(int rom, uint32_t at, uint8_t *out, uint32_t n) {
    if (!rom) { aa_bytes(at, out, n); return 1; }
    return rcp_cart_read(at, out, n);
}

uint32_t aa_frame_at(uint32_t ram, int *rom) { return frame_at(ram, rom); }
uint32_t aa_ledger_calls;

// Codebooks by address, hashed once a list: a list loads the same few over and over
static struct { uint32_t addr, len, hash, list; } books[16];
static uint32_t book_list;

uint32_t aa_book_hash(uint32_t addr, uint32_t len) {
    uint32_t slot = (addr >> 3) & 15;
    if (books[slot].addr == addr && books[slot].len == len && books[slot].list == book_list) return books[slot].hash;
    uint32_t h = 0x811C9DC5u;
    for (uint32_t b = 0; b < len; b++) h = (h ^ key_h->dram[(addr + b) ^ S8]) * 0x01000193u;
    books[slot].addr = addr, books[slot].len = len, books[slot].hash = h | 1, books[slot].list = book_list;
    return h | 1;
}

// The codebook the ADPCM command at a decodes with: the list's last LOADADPCM before it
static void book_of(uint32_t a, struct av_adpcm *d) {
    for (uint32_t i = a; i-- > 0;)
        if (cmds[i].kind == K_BOOK) {
            if (cmds[i].rlen > 512 || cmds[i].ram + cmds[i].rlen > 0x800000u) return;
            d->book_addr = cmds[i].ram, d->book_len = cmds[i].rlen;
            return;
        }
}

// The note's key from its A_INIT ADPCM: where its first frame came from, the frame's bytes and the codebook
static void note_key(struct hle_t *h, uint32_t a, uint32_t x, struct av_src *s) {
    const struct cmd *c = &cmds[a];
    uint32_t ram = frame_ram(a);
    struct av_adpcm d = { 0 };
    book_of(a, &d);
    if (ram == NONE || ram + 9 > 0x800000u || !d.book_len) return;
    s->book = aa_book_hash(d.book_addr, d.book_len), s->book_addr = d.book_addr, s->book_len = d.book_len;
    s->fs = c->af & 4 ? 5 : 9;
    int rom;
    uint32_t at = frame_at(ram, &rom);
    uint64_t k = 0xCBF29CE484222325ull;
    for (int b = 0; b < 4; b++) k = fnv8(k, at >> 8 * b);
    k = fnv8(k, (uint32_t)rom | (c->af & 4));
    for (uint32_t b = 0; b < 9; b++) k = fnv8(k, h->dram[(ram + b) ^ S8]);
    for (int b = 0; b < 4; b++) k = fnv8(k, s->book >> 8 * b);
    s->key = k, s->keyed = 1, s->rom = (uint8_t)rom, s->f0 = at, s->j0 = (uint16_t)((x - CW(c, 0).a) >> 1);
}

static int sort_cmds(const int16_t *a, uint32_t n, int16_t *out) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = i;
        for (; j > 0 && out[j - 1] > a[i]; j--) out[j] = out[j - 1];
        out[j] = a[i];
    }
    return (int)n;
}

// The ADPCM commands a voice's stream comes from, in list order, with where their frames came from
static void describe(const struct prov *p, struct av_src *s) {
    int16_t ad[AV_ADPCMS];
    s->nadpcm = (uint8_t)sort_cmds(p->ad, p->nad, ad);
    for (uint32_t q = 0; q < s->nadpcm; q++) {
        const struct cmd *c = &cmds[ad[q]];
        struct av_adpcm *d = &s->adpcm[q];
        d->ram = frame_ram((uint32_t)ad[q]), d->f = AV_UNRESOLVED, d->book = 0, d->book_addr = 0, d->book_len = 0;
        d->flags = c->af, d->frames = (uint16_t)(c->nr ? CR(c, 0).n / (c->af & 4 ? 5u : 9u) : 0);
        book_of((uint32_t)ad[q], d);
    }
    for (uint32_t g = 0; g < p->nseg; g++) {
        struct av_seg *t = &s->seg[s->nseg++];
        t->at = p->seg[g].at, t->n = p->seg[g].n, t->j = p->seg[g].j, t->adpcm = -1;
        for (uint32_t q = 0; q < s->nadpcm; q++)
            if (ad[q] == p->seg[g].cmd) t->adpcm = (int8_t)q;
    }
    if (s->nadpcm) s->fs = s->adpcm[0].flags & 4 ? 5 : 9;
}

// Each voice's samples traced to their source before liveness, so a voice the cache plays decodes nothing
__attribute__((noinline)) static void plan_voices(struct hle_t *h, int abi, uint32_t list, uint32_t n) {
    uint32_t nv = 0, seg[N_SEGMENTS] = { 0 };
    key_h = h;
    book_list++;
    int abi1 = abi == AA_ABI1;  // ABI1 walks the whole list for its SEGMENTs; the others only their voices
    for (uint32_t q = 0, j; abi1 ? q < n : q < nvres; q++) {
        j = abi1 ? q : vres[q];
        struct cmd *c = &cmds[j];
        int voice = (c->flags & (F_RES | F_CAP)) == (F_RES | F_CAP);
        if (!voice && !(abi1 && (c->flags & F_STATE))) continue;
        uint32_t w1 = *dram_u32(h, list + 8 * j), w2 = *dram_u32(h, list + 8 * j + 4);
        if (abi == AA_ABI1 && ((w1 >> 24) & 0x7F) == 7 && ((w2 >> 24) & 0x3F) < N_SEGMENTS) seg[(w2 >> 24) & 0x3F] = w2 & 0xFFFFFF;
        if (!voice) continue;
        c->vs = 0xFF;
        if (nv == MAXV) continue;
        uint32_t id, init, pitch;
        voice_args(abi, seg, w1, w2, &id, &init, &pitch);
        uint32_t ns = consumed(h, init, c->cnt, pitch, id), lo = c->in & 0xFFF;
        struct prov p;  // ad and seg are read only up to nad and nseg
        p.first_at = p.init_at = p.wrap = p.end = NONE, p.first_x = p.init_x = p.loop_x = 0;
        p.first = p.init = p.loop = -1, p.nad = p.nseg = p.foreign = 0;
        if (ns && lo + 2 * ns <= 0x1000) resolve(j, lo, lo + 2 * ns, 0, 0, &p);
        else if (ns) p.foreign = AA_FG_OTHER;
        // Kirby's last consumed sample is past its ADPCM output, stale: at pitches over 1 it only goes into the history
        if ((p.foreign == AA_FG_NONE || p.foreign == AA_FG_OTHER) && ns > 1 && lo + 2 * ns - 2 <= 0x1000) {
            p.first_at = p.init_at = p.wrap = p.end = NONE, p.first_x = p.init_x = p.loop_x = 0;
            p.first = p.init = p.loop = -1, p.nad = p.nseg = p.foreign = 0;
            resolve(j, lo, lo + 2 * ns - 2, 0, 0, &p);
        }
        if (!p.foreign && !p.nad && ns) p.foreign = AA_FG_CLEAR;
        aa_stats.foreign[p.foreign]++;
        struct av_src *s = &srcs[nv];
        memset(s, 0, __builtin_offsetof(struct av_src, adpcm));  // adpcm and seg are read only up to nadpcm and nseg
        s->wrap = p.wrap, s->end = p.end, s->f0 = NONE, s->fs = 9;
        s->ok = (uint8_t)(!p.foreign || p.foreign == AA_FG_CLEAR), s->silent = p.foreign == AA_FG_CLEAR, s->why = p.foreign;
        if (s->ok && p.nad) describe(&p, s);
        int starts = s->ok && p.init >= 0 && p.init_at == 0, keyon = init || starts;  // a new sample keys on, whatever the resampler says
        if (keyon && starts) note_key(h, (uint32_t)p.init, p.init_x, s);
        if (keyon && !init) c->flags |= F_KEYON, aa_stats.adpcm_keyons++;
        c->vs = (uint8_t)nv++;
        int plan = av_plan(id, keyon, ns, s);
        if (plan == AV_SKIP) c->flags |= F_RESID;
        if (plan == AV_PATCH && p.first >= 0 && !(cmds[p.first].af & (A_INIT | A_LOOP))) {
            int rom;
            int16_t st[16];
            uint32_t at = frame_at(frame_ram((uint32_t)p.first), &rom), addr = cmds[p.first].ram;
            if (addr + 32 <= 0x800000u && av_state(id, at, rom, st))  // the state the skipped lists never stored
                dram_store_u16(h, (const uint16_t *)st, addr, 16);
        }
    }
}

void alist_aica_run(struct hle_t *h, int abi, uint32_t hz) {
    uint32_t list = *dmem_u32(h, TASK_DATA_PTR) & 0xFFFFFF, n = *dmem_u32(h, TASK_DATA_SIZE) / 8, unattributed = 0;
    int nead = abi == AA_NEAD, naudio = abi == AA_NAUDIO;
    if (abi == AA_ABI1) memset(h->alist_audio.segments, 0, sizeof h->alist_audio.segments);
    find_voices(n);
    av_list_begin(hz);
    if (AV_CACHE) plan_voices(h, abi, list, n);
    eliminate(n);
    aa_stats.lists++;
    aa_stats.cmds += n;
    for (uint32_t i = 0; i < n; i++) {
        struct cmd *c = &cmds[i];
        if (!(c->flags & (F_KEEP | F_ENV | F_MIX)) && (c->flags & (F_RES | F_CAP)) != (F_RES | F_CAP)) continue;
        uint32_t w1 = *dram_u32(h, list + 8 * i), w2 = *dram_u32(h, list + 8 * i + 4);
        uint32_t flags = (w1 >> 16) & 0xFF, init = flags & A_INIT;
        if (c->flags & F_RES && c->flags & F_CAP) {
            uint32_t addr = res_addr(h, abi, w1, w2), in = c->in, cnt = c->cnt;
            uint32_t pitch = naudio ? ((w2 >> 14) & 0xFFFF) << 1 : (w1 & 0xFFFF) << 1;
            if (naudio) init = (w2 >> 30) & A_INIT;
            struct av_src *src = AV_CACHE && c->vs < MAXV ? &srcs[c->vs] : 0;
            uint32_t ns = consumed(h, init, cnt, pitch, addr), keyon = init || (c->flags & F_KEYON);
            if (c->flags & F_RESID) av_voice(addr, (int)keyon, 0, ns, cnt, pitch, src);
            else capture(h, addr, keyon, in, ns, cnt, pitch, src);
            if (c->flags & F_EXEC) {
                aa_stats.kept_resamples++;
                aa_stats.executed++;
                exec(h, abi, w1, w2);
            } else resample_state(h, init, in, cnt, pitch, addr);
            continue;
        }
        if (c->flags & F_ENV) {
            uint32_t addr = nead ? 0 : naudio ? w2 & 0xFFFFFF : alist_get_address(h, w2, h->alist_audio.segments, N_SEGMENTS), id;
            if (c->feed) {
                uint32_t f = list + 8 * (c->feed - 1);
                id = res_addr(h, abi, *dram_u32(h, f), *dram_u32(h, f + 4));
            } else {
                id = nead ? 0xFF000000u | unattributed : addr;
                unattributed++;
                capture(h, id, init, c->in, c->cnt >> 1, c->cnt, 0x10000, 0);
            }
            if (!(c->flags & F_KEEP)) {
                if (nead) envmix_nead(h, id, c->cnt >> 1);
                else if (naudio) envmix_naudio(h, id, init, addr, w1, c->cnt >> 1);
                else envmix_abi1(h, id, init, addr);
                continue;
            }
            aa_stats.executed++;
            exec(h, abi, w1, w2);
            if (nead) envmix_nead(h, id, 0);
            else if (naudio) envmix_naudio(h, id, 0, addr, w1, 0);
            else envmix_abi1(h, id, 0, addr);
            continue;
        }
        if ((c->flags & F_MIX) && !c->af) {
            uint32_t f = list + 8 * (c->feed - 1), g[2] = { 0, 0 };
            int32_t gain = (int16_t)c->cnt;
            g[c->out == NA_DR] = (uint32_t)(gain < 0 ? -gain : gain);
            for (uint32_t k = i + 1; k < n; k++)  // the voice's other side
                if ((cmds[k].flags & F_MIX) && cmds[k].feed == c->feed && cmds[k].out != c->out) {
                    gain = (int16_t)cmds[k].cnt;
                    g[cmds[k].out == NA_DR] = (uint32_t)(gain < 0 ? -gain : gain);
                    cmds[k].af = 1;
                    break;
                }
            av_gain(res_addr(h, abi, *dram_u32(h, f), *dram_u32(h, f + 4)), g[0], g[1], 0);
        }
        if (c->flags & F_KEEP) {
            if (!(c->flags & F_STATE)) aa_stats.executed++;
            exec(h, abi, w1, w2);
        }
    }
    av_list_end();
    rsp_break(h, SP_STATUS_TASKDONE);
}
