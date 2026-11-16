// RDP command state and TMEM: the commands Fast3D forwards, with loads following the RDP's TMEM layout.
#include "gfx.h"
#include "f3d.h"
#include "os.h"

struct tmem_rec gfx_tmem_recs[TMEM_RECS];
int gfx_tmem_nrecs;

static uint32_t fold(uint32_t h, uint32_t w) { return (h * 33) ^ w; }  // shifts and adds: no multiplier stall
static uint32_t rotl1(uint32_t h) { return h << 1 | h >> 31; }
// fold never moves a bit down; the rotate lets high data bits reach the low hash bits
static uint32_t fold_mix(uint32_t h, uint32_t w) { return rotl1(fold(h, w)); }

// A load hashes its source at once but copies into TMEM only when something reads TMEM (gfx_tmem_flush)
#define PEND_MAX 32  // above the live ones, which records reference: at most TMEM_RECS
struct pend { uint32_t w0, w1, timg, timg_w; uint16_t tmem, line; uint8_t op, siz; };
static struct pend pend[PEND_MAX];
static int npend;
static uint8_t cur_pend;  // the load being recorded, as its pend index + 1; 0 once in TMEM

static void tmem_flush(int all);

// A load wrote 64-bit words [lo, hi) with content hash h: older records lose the words it overwrote, so a texture
// in TMEM keys only on loads still visible there (a partly overwritten record keeps its hash: same data, same rest)
static void rec_add(uint32_t lo, uint32_t hi, uint32_t h) {
    if (hi > 512) {
        rec_add(lo, 512, h);
        rec_add(0, hi - 512, h ^ 0x9E3779B9u);
        return;
    }
    struct tmem_rec keep[2 * TMEM_RECS];
    int k = 0;
    for (int i = 0; i < gfx_tmem_nrecs; i++) {
        struct tmem_rec r = gfx_tmem_recs[i];
        if (r.hi <= lo || r.lo >= hi) {
            keep[k++] = r;
            continue;
        }
        if (r.lo < lo) keep[k] = r, keep[k++].hi = (uint16_t)lo;
        if (r.hi > hi) keep[k] = r, keep[k++].lo = (uint16_t)hi;
    }
    if (k >= TMEM_RECS) {  // too fragmented: one record for all of TMEM, the new data included
        tmem_flush(1);
        const uint32_t *w = (const uint32_t *)g.tmem;
        uint32_t all = 0x811C9DC5u;
        for (int i = 0; i < 1024; i++) all = fold_mix(all, w[i]);
        gfx_tmem_recs[0].lo = 0, gfx_tmem_recs[0].hi = 512, gfx_tmem_recs[0].hash = all, gfx_tmem_recs[0].pend = 0;
        gfx_tmem_nrecs = 1;
        return;
    }
    __builtin_memcpy(gfx_tmem_recs, keep, k * sizeof keep[0]);
    gfx_tmem_recs[k].lo = (uint16_t)lo, gfx_tmem_recs[k].hi = (uint16_t)hi, gfx_tmem_recs[k].hash = h;
    gfx_tmem_recs[k].pend = cur_pend;
    gfx_tmem_nrecs = k + 1;
}

static uint32_t rd32_any(uint32_t phys) {
    if (!(phys & 3)) return gfx_rd32(phys);
    return (uint32_t)gfx_rd8(phys) << 24 | (uint32_t)gfx_rd8(phys + 1) << 16 | (uint32_t)gfx_rd8(phys + 2) << 8 | gfx_rd8(phys + 3);
}

#define LANE_B 0x9E3779B9u

// n 32-bit source words into h: one chain per half of a 64-bit word, so the two overlap
static void hash_src(uint32_t phys, uint32_t n, uint32_t *h) {
    uint32_t a = *h, b = *h ^ LANE_B;
    if (phys & 3) {
        for (uint32_t k = 0; k < n; k++) a = fold_mix(a, rd32_any(phys + 4 * k));
    } else {
        const uint32_t *s = (const uint32_t *)(0x80000000u + phys + t_rdram_bias);
        for (; n >= 4; n -= 4, s += 4) {  // pref two lines ahead: each source line misses once
            uint32_t a0 = s[0], b0 = s[1], a1 = s[2], b1 = s[3];
            __builtin_prefetch(s + 16);
            a = rotl1(fold(fold(a, a0), a1)), b = rotl1(fold(fold(b, b0), b1));
        }
        for (; n; n--, s++) a = fold_mix(a, *s);
    }
    *h = fold_mix(a, b);
}

// n 64-bit words from RDRAM into TMEM words from `word` on; odd rows have their 32-bit halves swapped
static void copy64(uint32_t word, uint32_t phys, uint32_t n, int swap) {
    const uint32_t *s = (const uint32_t *)(0x80000000u + (phys & ~3u) + t_rdram_bias);
    uint32_t *d = (uint32_t *)g.tmem, m = (phys & 3) * 8;
    if (!m) {
        for (uint32_t i = 0; i < n; i++, s += 2) {
            uint32_t *e = d + ((word + i) & 0x1FF) * 2;
            e[swap] = s[0], e[!swap] = s[1];
        }
        return;
    }
    uint32_t cur = s[0];  // the last aligned word read still holds the run's last byte
    for (uint32_t i = 0; i < n; i++, s += 2) {
        uint32_t w1 = s[1], w2 = s[2], *e = d + ((word + i) & 0x1FF) * 2;
        e[swap] = cur << m | w1 >> (32 - m), e[!swap] = w1 << m | w2 >> (32 - m);
        cur = w2;
    }
}

// n 32-bit texels from texel16 on: red/green go to the low half of TMEM, blue/alpha to the high half
static void copy32(uint32_t texel16, uint32_t phys, uint32_t n, int swap) {
    uint16_t *lo = (uint16_t *)g.tmem, *hi = lo + 0x400;
    uint32_t x = swap ? 3 : 1;  // TMEM's 16-bit halves sit word-swapped too: ^1
    for (uint32_t k = 0; k < n; k++) {
        uint32_t w = rd32_any(phys + k * 4), i = ((texel16 + k) ^ x) & 0x3FF;
        lo[i] = (uint16_t)(w >> 16), hi[i] = (uint16_t)w;
    }
}

// Words of a LoadBlock from the one with dxt count tcount on that share its row parity; per is 2048 / dxt when exact
static uint32_t parity_run(uint32_t tcount, uint32_t dxt, uint32_t per, uint32_t left) {
    if (!dxt) return left;
    if (per) return per < left ? per : left;
    uint32_t run = 0;
    for (uint32_t c = tcount, end = (tcount | 2047) + 1; run < left && c < end; c += dxt) run++;  // no divide
    return run;
}

static void rec_add32(uint32_t first16, uint32_t end16, uint32_t h) {
    uint32_t lo = (first16 & 0x3FF) >> 2, hi = lo + ((end16 - first16 + 3) >> 2);
    if (hi > 256) lo = 0, hi = 256;  // wrapped inside the half: take the whole half
    rec_add(lo, hi, h);
    rec_add(lo + 256, hi + 256, h);
}

// Each load walks its source twice: with h, hashing it as the command arrives; without, copying it into TMEM
static void block_walk(const struct pend *p, uint32_t *h) {
    uint32_t uls = (p->w0 >> 12) & 0xFFF, ult = p->w0 & 0xFFF, lrs = (p->w1 >> 12) & 0xFFF, dxt = p->w1 & 0xFFF;
    uint32_t siz = p->siz, src = p->timg + (((ult * p->timg_w + uls) << siz) >> 1);
    uint32_t words = ((((lrs - uls + 1) << siz) >> 1) + 7) >> 3, per = dxt && !(2048 % dxt) ? 2048 / dxt : 0;
    if (h) {
        *h ^= dxt;
        hash_src(src, words * 2, h);
        return;
    }
    for (uint32_t i = 0, run, tcount = 0; i < words; i += run, tcount += run * dxt) {
        int odd = (tcount >> 11) & 1;
        run = parity_run(tcount, dxt, per, words - i);
        if (siz == 3) copy32(p->tmem * 4 + i * 2, src + i * 8, run * 2, odd);
        else copy64(p->tmem + i, src + i * 8, run, odd);
    }
}

static void tile_walk(const struct pend *p, uint32_t *h) {
    uint32_t uls = ((p->w0 >> 12) & 0xFFF) >> 2, ult = (p->w0 & 0xFFF) >> 2, siz = p->siz;
    uint32_t texels = (((p->w1 >> 12) & 0xFFF) >> 2) - uls + 1, rows = ((p->w1 & 0xFFF) >> 2) - ult + 1;
    uint32_t words = (((texels << siz) >> 1) + 7) >> 3;
    for (uint32_t row = 0; row < rows; row++) {
        uint32_t src = p->timg + ((((ult + row) * p->timg_w + uls) << siz) >> 1);
        if (h) hash_src(src, siz == 3 ? texels : words * 2, h);
        else if (siz == 3) copy32(p->tmem * 4 + row * p->line * 4, src, texels, row & 1);
        else copy64(p->tmem + row * p->line, src, words, row & 1);
    }
}

// Each 16-bit TLUT entry is stored four times across a 64-bit TMEM word
static void tlut_walk(const struct pend *p, uint32_t *h) {
    uint32_t uls = ((p->w0 >> 12) & 0xFFF) >> 2, n = (((p->w1 >> 12) & 0xFFF) >> 2) - uls + 1;
    uint32_t src = p->timg + (uls << 1);
    if (h) {
        for (uint32_t i = 0; i < n; i++) *h = fold_mix(*h, gfx_rd16(src + 2 * i));
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = gfx_rd16(src + 2 * i), *d = (uint32_t *)g.tmem + ((p->tmem + i) & 0x1FF) * 2;
        d[0] = d[1] = e << 16 | e;
    }
}

static void replay(const struct pend *p) {
    if (p->op == G_LOADBLOCK) block_walk(p, 0);
    else if (p->op == G_LOADTILE) tile_walk(p, 0);
    else tlut_walk(p, 0);
}

// Copies the queued loads into TMEM in order; all = 0 skips those that records show are overwritten
static void tmem_flush(int all) {
    uint32_t live = all ? ~0u : 0;
    for (int i = 0; i < gfx_tmem_nrecs; i++) {
        if (!all && gfx_tmem_recs[i].pend) live |= 1u << (gfx_tmem_recs[i].pend - 1);
        gfx_tmem_recs[i].pend = 0;
    }
    for (int i = 0; i < npend; i++)
        if (live >> i & 1) replay(&pend[i]);
    npend = 0;
    cur_pend = 0;
}

void gfx_tmem_materialize(void) {
    if (npend) tmem_flush(0);
}

// Source hashes of this slice's loads: RDRAM only changes while the CPU runs, so they hold until the task parks
#define MEMO_BITS 8
#define MEMO_SLOTS (1 << MEMO_BITS)  // a Cruis'n race frame loads 245 distinct sources
struct memo { uint32_t w0, w1, timg, timg_w, h, gen; uint16_t line; uint8_t op, siz; };
static struct memo memo[MEMO_SLOTS];
static uint32_t memo_gen = 1;
uint32_t gfx_load_hashed;

void gfx_tmem_flush(void) {
    gfx_tmem_materialize();
    memo_gen++;
}

static void hash_load(const struct pend *p, void (*walk)(const struct pend *, uint32_t *), uint32_t *h) {
    uint32_t w1 = p->w1 & 0xF8FFFFFFu;  // the tile number doesn't change the source
    struct memo *m = &memo[((p->timg ^ p->w0 * 0x9E3779B1u ^ w1 * 0x85EBCA77u) * 0x9E3779B1u) >> (32 - MEMO_BITS)];
    if (m->gen == memo_gen && m->op == p->op && m->w0 == p->w0 && m->w1 == w1 && m->timg == p->timg
        && m->timg_w == p->timg_w && m->siz == p->siz && m->line == p->line) {
        *h = m->h;
        return;
    }
    walk(p, h);
    gfx_load_hashed++;
    m->op = p->op, m->w0 = p->w0, m->w1 = w1, m->timg = p->timg, m->timg_w = p->timg_w, m->siz = p->siz;
    m->line = p->line, m->h = *h, m->gen = memo_gen;
}

// Queues the load for tile t and sets cur_pend for its records; a full queue first drops the overwritten loads
static void pend_push(uint32_t op, uint32_t w0, uint32_t w1, const struct rdp_tile *t) {
    if (npend == PEND_MAX) {
        uint32_t live = 0;
        uint8_t map[PEND_MAX];
        int k = 0;
        for (int i = 0; i < gfx_tmem_nrecs; i++)
            if (gfx_tmem_recs[i].pend) live |= 1u << (gfx_tmem_recs[i].pend - 1);
        for (int i = 0; i < PEND_MAX; i++) {
            map[i] = live >> i & 1 ? (uint8_t)(k + 1) : 0;
            if (map[i]) pend[k++] = pend[i];
        }
        for (int i = 0; i < gfx_tmem_nrecs; i++)
            if (gfx_tmem_recs[i].pend) gfx_tmem_recs[i].pend = map[gfx_tmem_recs[i].pend - 1];
        npend = k;
    }
    struct pend *p = &pend[npend++];
    p->w0 = w0, p->w1 = w1, p->timg = g.timg, p->timg_w = g.timg_w;
    p->tmem = t->tmem, p->line = t->line, p->op = (uint8_t)op, p->siz = (uint8_t)g.timg_siz;
    cur_pend = (uint8_t)npend;
}

// The handlers below gfx_rdp tail-calls: its common path then saves no registers
static __attribute__((noinline)) void load_block(uint32_t w0, uint32_t w1) {
    struct rdp_tile *t = &g.tile[(w1 >> 24) & 7];
    uint32_t uls = (w0 >> 12) & 0xFFF, ult = w0 & 0xFFF, lrs = (w1 >> 12) & 0xFFF, dxt = w1 & 0xFFF;
    if (lrs < uls) os_die("rdp: LoadBlock with lrs < uls", w1);
    uint32_t siz = g.timg_siz, src = g.timg + (((ult * g.timg_w + uls) << siz) >> 1);
    uint32_t words = ((((lrs - uls + 1) << siz) >> 1) + 7) >> 3, h = 0x811C9DC5u;
    gfx_need(src, words * 8, "rdp: LoadBlock source out of RDRAM");
    pend_push(G_LOADBLOCK, w0, w1, t);
    hash_load(&pend[npend - 1], block_walk, &h);
    if (siz == 3) rec_add32(t->tmem * 4, t->tmem * 4 + words * 2, h);
    else rec_add(t->tmem, t->tmem + words, h);
    t->uls = (uint16_t)uls, t->ult = (uint16_t)ult, t->lrs = (uint16_t)lrs, t->lrt = (uint16_t)dxt;  // as angrylion's load_block
    g.tmem_gen++;
    gfx_state_changed();
}

static __attribute__((noinline)) void load_tile(uint32_t w0, uint32_t w1) {
    struct rdp_tile *t = &g.tile[(w1 >> 24) & 7];
    uint32_t uls = ((w0 >> 12) & 0xFFF) >> 2, ult = (w0 & 0xFFF) >> 2;
    uint32_t lrs = ((w1 >> 12) & 0xFFF) >> 2, lrt = (w1 & 0xFFF) >> 2, siz = g.timg_siz;
    if (lrs < uls || lrt < ult) os_die("rdp: LoadTile with an empty rectangle", w1);
    uint32_t texels = lrs - uls + 1, rows = lrt - ult + 1, h = 0x811C9DC5u ^ t->line;
    uint32_t words = (((texels << siz) >> 1) + 7) >> 3, bytes = siz == 3 ? texels * 4 : words * 8;
    for (uint32_t row = 0; row < rows; row++)
        gfx_need(g.timg + ((((ult + row) * g.timg_w + uls) << siz) >> 1), bytes, "rdp: LoadTile source out of RDRAM");
    pend_push(G_LOADTILE, w0, w1, t);
    hash_load(&pend[npend - 1], tile_walk, &h);
    if (siz == 3) rec_add32(t->tmem * 4, t->tmem * 4 + (rows - 1) * t->line * 4 + texels, h);
    else rec_add(t->tmem, t->tmem + (rows - 1) * t->line + words, h);
    t->uls = (w0 >> 12) & 0xFFF, t->ult = w0 & 0xFFF, t->lrs = (w1 >> 12) & 0xFFF, t->lrt = w1 & 0xFFF;  // the RDP keeps them
    g.tmem_gen++;
    gfx_state_changed();
}

static __attribute__((noinline)) void load_tlut(uint32_t w0, uint32_t w1) {
    struct rdp_tile *t = &g.tile[(w1 >> 24) & 7];
    uint32_t uls = ((w0 >> 12) & 0xFFF) >> 2, lrs = ((w1 >> 12) & 0xFFF) >> 2;
    if (lrs < uls) os_die("rdp: LoadTLUT with lrs < uls", w1);
    uint32_t n = lrs - uls + 1, h = 0x811C9DC5u;
    gfx_need(g.timg + (uls << 1), n * 2, "rdp: LoadTLUT source out of RDRAM");
    pend_push(G_LOADTLUT, w0, w1, t);
    hash_load(&pend[npend - 1], tlut_walk, &h);
    rec_add(t->tmem & 0x1FF, (t->tmem & 0x1FF) + n, h);
    g.tmem_gen++;
}

GFX_HOT(00) __attribute__((noinline)) static void set_tile(uint32_t w0, uint32_t w1) {
    struct rdp_tile *t = &g.tile[(w1 >> 24) & 7];
    __asm__("" : "+r"(t));  // one base register: SH-4 byte stores reach 15 bytes, GCC otherwise rebuilds g + 0x21B8 + k per field
    t->fmt = (w0 >> 21) & 7;
    t->siz = (w0 >> 19) & 3;
    t->line = (w0 >> 9) & 0x1FF;
    t->tmem = w0 & 0x1FF;
    t->pal = (w1 >> 20) & 0xF;
    t->cmt = (w1 >> 18) & 3;
    t->maskt = (w1 >> 14) & 0xF;
    t->shiftt = (w1 >> 10) & 0xF;
    t->cms = (w1 >> 8) & 3;
    t->masks = (w1 >> 4) & 0xF;
    t->shifts = w1 & 0xF;
    gfx_state_changed();
}

GFX_HOT(00) __attribute__((noinline)) static void set_tile_size(uint32_t w0, uint32_t w1) {
    struct rdp_tile *t = &g.tile[(w1 >> 24) & 7];
    __asm__("" : "+r"(t));
    t->uls = (w0 >> 12) & 0xFFF;
    t->ult = w0 & 0xFFF;
    t->lrs = (w1 >> 12) & 0xFFF;
    t->lrt = w1 & 0xFFF;
    gfx_state_changed();
}

GFX_HOT(00) void gfx_rdp(uint32_t w0, uint32_t w1) {  // beside rsp_gfx_task: they alternate on every RDP command
    if (g.rect_pending) {
        g.rect_pending = 0;
        gfx_texrect(g.rect_w0, g.rect_w1, w0, w1);
        return;
    }
    switch (w0 >> 24) {
    case G_TEXRECT:
    case G_TEXRECTFLIP:
        g.rect_w0 = w0, g.rect_w1 = w1, g.rect_pending = 1;
        return;
    case G_NOOP:
    case G_RDPLOADSYNC:
    case G_RDPPIPESYNC:
    case G_RDPTILESYNC:
    case G_RDPFULLSYNC:
        return;
    case G_SETKEYGB: g.keygb = w1; return;
    case G_SETKEYR: g.keyr = w1; return;
    case G_SETCONVERT: g.convert_lo = w1; return;  // K4/K5; K0-K3 (YUV) are never applied
    case G_SETSCISSOR:
        g.scis_ulx = (w0 >> 12) & 0xFFF, g.scis_uly = w0 & 0xFFF;
        g.scis_lrx = (w1 >> 12) & 0xFFF, g.scis_lry = w1 & 0xFFF;
        gfx_scissor();
        return;
    case G_SETPRIMDEPTH: g.prim_z = (uint16_t)(w1 >> 16), g.prim_dz = (uint16_t)w1; gfx_state_changed(); return;
    case G_RDPSETOTHERMODE:
        g.rdp_omh = w0 & 0x00FFFFFF, g.rdp_oml = w1;
        gfx_state_changed();
        return;
    case G_LOADTLUT: load_tlut(w0, w1); return;
    case G_SETTILESIZE: set_tile_size(w0, w1); return;
    case G_LOADBLOCK: load_block(w0, w1); return;
    case G_LOADTILE: load_tile(w0, w1); return;
    case G_SETTILE: set_tile(w0, w1); return;
    case G_FILLRECT: gfx_fillrect(w0, w1); return;
    case G_SETFILLCOLOR: g.fill = w1; return;
    case G_SETFOGCOLOR: g.fog = w1; gfx_state_changed(); return;
    case G_SETBLENDCOLOR: g.blend = w1; gfx_state_changed(); return;
    case G_SETPRIMCOLOR: g.prim = w1, g.prim_lod = w0 & 0xFFFF; gfx_state_changed(); return;
    case G_SETENVCOLOR: g.env = w1; gfx_state_changed(); return;
    case G_SETCOMBINE: g.cc_hi = w0 & 0x00FFFFFF, g.cc_lo = w1; gfx_state_changed(); return;
    case G_SETTIMG:
        g.timg = w1 & 0x00FFFFFF, g.timg_fmt = (w0 >> 21) & 7, g.timg_siz = (w0 >> 19) & 3, g.timg_w = (w0 & 0x3FF) + 1;
        return;
    case G_SETZIMG: g.zimg = w1 & 0x00FFFFFF; return;
    case G_SETCIMG:
        g.cimg = w1 & 0x00FFFFFF, g.cimg_siz = (w0 >> 19) & 3, g.cimg_w = (w0 & 0x3FF) + 1;
        return;
    default:
        os_die("rdp: command the RDP HLE doesn't model", w0);
    }
}
