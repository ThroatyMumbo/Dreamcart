// Tile decode from TMEM to PVR 16-bit texels, and the texture cache in front of the sink.
#include "gfx.h"
#include "gfx_tex.h"
#include "gfx_sink.h"
#include "os.h"

#define TEX_SLOTS 512
#define TEX_PROBES 128  // MK64's title keeps ~300 textures live; 32 probes filled windows
#define TEX_KEEP_FRAMES 2  // the frame being built and the one the PVR may still be rendering
#define TEX_MAX_TEXELS (256 * 256)
#define TEX_EXPAND_MAX 8192  // Kirby's faces expand to 256x32 at most; its water and planet tiles (64 over 512) wrap
#define OMH_TLUT_SHIFT 14
#define OMH_FILT_SHIFT 12

#define KEY_WORDS 10
struct tex_key { uint32_t w[KEY_WORDS]; };  // 3, 4, 5 and 9 hash TMEM content; 6, 7, 8 describe an alpha tile
struct tex_slot { struct tex_key k; uint32_t used; uint16_t w, h; uint8_t fmt, live; };

static struct tex_slot slot[TEX_SLOTS];
static uint32_t rgba[TEX_MAX_TEXELS];
static uint16_t texels[TEX_MAX_TEXELS] __attribute__((aligned(32)));
typedef uint32_t __attribute__((may_alias)) texel_pair;  // two texels stored at once
static struct tex_key last_key;
static uint32_t last_gen = 0xFFFFFFFFu;
static int last_slot = -1;
static uint32_t tex_frame = TEX_KEEP_FRAMES;
uint32_t gfx_tex_uploads, gfx_tex_lookups, gfx_tex_forced;

// The content key of TMEM words [lo, hi): every load record that overlaps them, oldest first
static uint32_t span_key(uint32_t lo, uint32_t hi) {
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < gfx_tmem_nrecs; i++) {
        const struct tmem_rec *r = &gfx_tmem_recs[i];
        if (r->lo < hi && r->hi > lo) h = ((((h ^ r->lo) * 0x01000193u) ^ r->hi) * 0x01000193u ^ r->hash) * 0x01000193u;
    }
    return h;
}

static uint32_t from5551(uint16_t c) {
    uint32_t r = (c >> 11) & 31, gg = (c >> 6) & 31, b = (c >> 1) & 31;
    return (r << 3 | r >> 2) << 24 | (gg << 3 | gg >> 2) << 16 | (b << 3 | b >> 2) << 8 | ((c & 1) ? 0xFF : 0);
}
static uint32_t gray(uint32_t i, uint32_t a) { return i << 24 | i << 16 | i << 8 | a; }

static uint32_t tlut(uint32_t idx, uint32_t mode) {
    uint16_t e = tm16(0x800 + idx * 8);  // entry idx of the upper half, first of its four copies
    return mode == 3 ? gray(e >> 8, e & 0xFF) : from5551(e);
}

// One texel of tile t at column s, row r of the image, as RGBA8888
static uint32_t texel(const struct rdp_tile *t, uint32_t s, uint32_t r, uint32_t tlut_mode) {
    uint32_t row = t->tmem * 8 + r * t->line * 8, odd = r & 1;
    if (t->siz == 3) {
        uint32_t i = ((t->tmem * 4 + r * t->line * 4 + s) ^ (odd ? 2 : 0)) & 0x3FF;
        uint16_t rg = tm16(i * 2), ba = tm16(0x800 + i * 2);
        if (t->fmt == 0) return (uint32_t)rg << 16 | ba;
        return gray(rg >> 8, rg >> 8);
    }
    uint32_t a = (row + ((s << t->siz) >> 1)) ^ (odd ? 4 : 0);
    if (t->siz == 2) {
        uint16_t c = tm16(a & ~1u);
        if (tlut_mode && t->fmt == 2) return tlut(c >> 8, tlut_mode);
        if (t->fmt == 3) return gray(c >> 8, c & 0xFF);
        if (t->fmt == 4) return gray(c >> 8, c >> 8);
        return from5551(c);
    }
    uint32_t v = tm8(a);
    if (t->siz == 0) v = (s & 1) ? v & 0xF : v >> 4;
    if (tlut_mode && t->siz <= 1) return tlut(t->siz == 0 ? (uint32_t)t->pal * 16 + v : v, tlut_mode);
    if (t->siz == 1) {
        if (t->fmt == 3) return gray((v >> 4) * 17, (v & 15) * 17);
        return gray(v, v);
    }
    if (t->fmt == 3) {
        uint32_t i = (v >> 1) & 7;
        return gray((i << 5) | (i << 2) | (i >> 1), (v & 1) ? 0xFF : 0);
    }
    return gray(v * 17, v * 17);
}

static uint32_t pot(uint32_t v) {
    uint32_t p = 8;
    while (p < v) p <<= 1;
    return p;
}

// The RDP clamps to the tile, then masks: a clamped tile wider than its mask repeats (or mirrors) inside the tile only,
// so it decodes at the tile's size with texel indices folded by the mask (*wrap: mask width, *wflip: mirrored)
static void axis(uint32_t lo, uint32_t hi, uint32_t mask, uint32_t cm, uint32_t *iw, uint8_t *clamp, uint8_t *flip,
                 uint32_t *wrap, uint8_t *wflip) {
    uint32_t size = hi >= lo ? ((hi - lo) >> 2) + 1 : 1, mw = 1u << mask;
    *flip = 0, *wrap = 0, *wflip = 0;
    if ((cm & 2) && (!mask || size <= mw)) *iw = size, *clamp = 1;
    else if (cm & 2) *iw = size, *clamp = 1, *wrap = mw, *wflip = (cm & 1) != 0;
    else if (mask) *iw = mw, *clamp = 0, *flip = (cm & 1) != 0;
    else *iw = size, *clamp = 1;
}

// The decimated size of an axis, or 1 when it can't be: a mirror, or a wrap the step doesn't divide
static uint32_t dec_axis(uint32_t *size, uint8_t clamp, uint8_t flip, uint32_t step, uint32_t phase) {
    if (step <= 1 || flip || (!clamp && *size % step)) return 1;
    *size = clamp ? (*size > phase ? (*size - phase + step - 1) / step : 1) : *size / step;
    return step;
}

static uint8_t choose_fmt(uint32_t n) {
    int opaque = 1, binary = 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = rgba[i] & 0xFF;
        if (a != 0xFF) opaque = 0;
        if (a != 0 && a != 0xFF) binary = 0;
    }
    return opaque ? TF_RGB565 : binary ? TF_ARGB1555 : TF_ARGB4444;
}

static uint16_t to16(uint32_t c, uint8_t fmt) {
    uint32_t r = c >> 24, gg = (c >> 16) & 0xFF, b = (c >> 8) & 0xFF, a = c & 0xFF;
    if (fmt == TF_RGB565) return (uint16_t)((r >> 3) << 11 | (gg >> 2) << 5 | b >> 3);
    if (fmt == TF_ARGB1555) return (uint16_t)((a ? 0x8000 : 0) | (r >> 3) << 10 | (gg >> 3) << 5 | b >> 3);
    return (uint16_t)((a >> 4) << 12 | (r >> 4) << 8 | (gg >> 4) << 4 | b >> 4);
}

// RGBA16 straight to PVR 16-bit: 5551 alpha is binary, so it lands as ARGB1555, or RGB565 when all opaque
// A TMEM 32-bit word holds texels 2j (high half) and 2j + 1 (low half) of a row
static inline uint32_t tm32(uint32_t a) { return *(const uint32_t *)(g.tmem + (a & 0xFFC)); }

static uint8_t decode_rgba16(const struct rdp_tile *t, uint32_t iw, uint32_t ih, uint32_t pw, uint32_t ph) {
    uint32_t all = 0x10001, pairs = (iw + 1) / 2;
    for (uint32_t r = 0; r < ih; r++) {
        uint32_t row = t->tmem * 8 + r * t->line * 8, sw = (r & 1) ? 4 : 0;
        for (uint32_t j = 0; j + 1 < pairs; j++) all &= tm32((row + 4 * j) ^ sw);
        all &= tm32((row + 4 * (pairs - 1)) ^ sw) | (iw & 1);  // an odd width's last low half is past the tile
    }
    int opaque = (all & 0x10001) == 0x10001;
    for (uint32_t y = 0; y < ph; y++) {
        uint16_t *o = texels + y * pw;
        if (y >= ih) {
            __builtin_memcpy(o, o - pw, pw * 2);
            continue;
        }
        uint32_t row = t->tmem * 8 + y * t->line * 8, sw = (y & 1) ? 4 : 0;
        texel_pair *o32 = (texel_pair *)o;
        if (opaque)
            for (uint32_t j = 0; j < pairs; j++) {
                uint32_t w = tm32((row + 4 * j) ^ sw);
                w = (w & 0xF800F800u) | (w >> 6 & 0x001F001Fu) << 6 | (w >> 10 & 0x00010001u) << 5 | (w >> 1 & 0x001F001Fu);
                o32[j] = w << 16 | w >> 16;
            }
        else
            for (uint32_t j = 0; j < pairs; j++) {
                uint32_t w = tm32((row + 4 * j) ^ sw);
                w = (w >> 1 & 0x7FFF7FFFu) | (w & 0x00010001u) << 15;
                o32[j] = w << 16 | w >> 16;
            }
        for (uint32_t x = iw; x < pw; x++) o[x] = o[iw - 1];
    }
    return opaque ? TF_RGB565 : TF_ARGB1555;
}

static int key_eq(const struct tex_key *a, const struct tex_key *b) {
    for (int i = 0; i < KEY_WORDS; i++) if (a->w[i] != b->w[i]) return 0;
    return 1;
}

// TMEM words [lo, hi) of a tile's image rows, with the upper half for 32-bit and the palette under a TLUT
static uint32_t tile_spans(const struct rdp_tile *t, uint32_t rows, uint32_t tlut_mode, uint32_t *upper, uint32_t *pal) {
    uint32_t lo = t->tmem, hi = lo + (uint32_t)t->line * rows;
    if (hi > 512 || hi == lo) lo = 0, hi = 512;
    *upper = t->siz == 3 ? span_key(lo + 256, hi + 256) : 0;
    *pal = tlut_mode ? (t->siz == 0 ? span_key(256 + t->pal * 16u, 272 + t->pal * 16u) : span_key(256, 512)) : 0;
    return span_key(lo, hi);
}

// Image coordinate v on an axis of n texels: clamped, or wrapped by the power-of-two mask, mirrored when flip
static uint32_t fold(int32_t v, uint32_t n, uint8_t clamp, uint8_t flip) {
    if (clamp) return v < 0 ? 0 : (uint32_t)v >= n ? n - 1 : (uint32_t)v;
    uint32_t u = (uint32_t)v & (n - 1);
    return flip && ((uint32_t)v & n) ? n - 1 - u : u;
}

// A tile's image as decoded: its size, the power-of-two size it pads to, each axis's fold, and the decode's step and phase
struct tile_geom {
    uint32_t iw, ih, ih_tmem, pw, ph, ks, kt, ps, pt, wrap_s, wrap_t;
    uint8_t wflip_s, wflip_t;
};

// The alpha tile, sampled at the same s, t through its own origin, mask and clamp; at is 0 for none
struct alpha_src {
    const struct rdp_tile *at;
    uint32_t aw, ah, awrap_s, awrap_t;
    uint8_t aclamp_u, aclamp_v, aflip_u, aflip_v, awflip_s, awflip_t;
    int32_t adx, ady;
};

static void tile_geometry(const struct rdp_tile *t, const struct tex_dec *want, struct tex_bind *b, struct tile_geom *gm) {
    uint32_t iw, ih, wrap_s, wrap_t;
    uint8_t wflip_s, wflip_t;
    axis(t->uls, t->lrs, t->masks, t->cms, &iw, &b->clamp_u, &b->flip_u, &wrap_s, &wflip_s);
    axis(t->ult, t->lrt, t->maskt, t->cmt, &ih, &b->clamp_v, &b->flip_v, &wrap_t, &wflip_t);
    if ((wrap_s || wrap_t) && pot(iw) * pot(ih) > TEX_EXPAND_MAX) {  // too big to expand: wrap at the mask, as unclamped
        if (wrap_s) iw = wrap_s, b->clamp_u = 0, b->flip_u = wflip_s, wrap_s = 0, wflip_s = 0;
        if (wrap_t) ih = wrap_t, b->clamp_v = 0, b->flip_v = wflip_t, wrap_t = 0, wflip_t = 0;
    }
    uint32_t ih_tmem = wrap_t && wrap_t < ih ? wrap_t : ih;  // the rows read: a folded axis reads only the mask's
    uint32_t ks = dec_axis(&iw, b->clamp_u, b->flip_u, want->step_s, want->phase_s);
    uint32_t kt = dec_axis(&ih, b->clamp_v, b->flip_v, want->step_t, want->phase_t);
    uint32_t ps = ks > 1 ? want->phase_s : 0, pt = kt > 1 ? want->phase_t : 0;
    b->step_s = (uint8_t)ks, b->step_t = (uint8_t)kt, b->phase_s = (uint8_t)ps, b->phase_t = (uint8_t)pt;
    uint32_t pw = pot(iw), ph = pot(ih);
    if (pw > 1024 || ph > 1024 || pw * ph > TEX_MAX_TEXELS) os_die("gfx tex: tile image larger than the decode buffer", pw << 16 | ph);
    if (pw != iw) b->clamp_u = 1;
    if (ph != ih) b->clamp_v = 1;
    b->w = (uint16_t)pw, b->h = (uint16_t)ph;
    b->filter = ((g.rdp_omh >> OMH_FILT_SHIFT) & 3) != 0;
    b->uls = t->uls, b->ult = t->ult, b->shifts = t->shifts, b->shiftt = t->shiftt;
    *gm = (struct tile_geom){ iw, ih, ih_tmem, pw, ph, ks, kt, ps, pt, wrap_s, wrap_t, wflip_s, wflip_t };
}

// A different shift can't merge: that alpha tile is left out
static void alpha_source(const struct rdp_tile *t, int alpha_tile, struct alpha_src *a) {
    *a = (struct alpha_src){ 0 };
    const struct rdp_tile *at = alpha_tile >= 0 ? &g.tile[alpha_tile & 7] : 0;
    if (!at || at->shifts != t->shifts || at->shiftt != t->shiftt) return;
    a->at = at;
    axis(at->uls, at->lrs, at->masks, at->cms, &a->aw, &a->aclamp_u, &a->aflip_u, &a->awrap_s, &a->awflip_s);
    axis(at->ult, at->lrt, at->maskt, at->cmt, &a->ah, &a->aclamp_v, &a->aflip_v, &a->awrap_t, &a->awflip_t);
    a->adx = (int32_t)(t->uls >> 2) - (int32_t)(at->uls >> 2), a->ady = (int32_t)(t->ult >> 2) - (int32_t)(at->ult >> 2);
}

// Words 0-2 and 6-8 of the cache key; 3-5 and 9 hash TMEM, and the last bind's stand in for them until TMEM changes
static void tex_key_of(const struct rdp_tile *t, const struct tile_geom *gm, const struct alpha_src *a,
                       const struct tex_bind *b, struct tex_fx fx, uint32_t tlut_mode, struct tex_key *k) {
    const struct rdp_tile *at = a->at;
    k->w[0] = (uint32_t)t->fmt | t->siz << 3 | (uint32_t)t->pal << 5 | tlut_mode << 9 | (uint32_t)t->line << 11 | (uint32_t)t->tmem << 20;
    k->w[1] = gm->iw | (gm->wrap_s ? t->masks | gm->wflip_s << 4 : 0) << 11 | gm->ih << 16 |
              (gm->wrap_t ? t->maskt | gm->wflip_t << 4 : 0) << 27;
    k->w[2] = (uint32_t)b->clamp_u | b->clamp_v << 1 | gm->ks << 2 | gm->kt << 6 | gm->ps << 10 | gm->pt << 14 |
              (uint32_t)(fx.alpha_ref + 1) << 18 | (uint32_t)fx.invert << 27;
    k->w[6] = at ? 1u | (uint32_t)at->fmt << 1 | at->siz << 4 | (uint32_t)at->pal << 6 | (uint32_t)at->line << 10 | (uint32_t)at->tmem << 19 : 0;
    k->w[7] = (uint32_t)(a->adx & 0xFFFF) | (uint32_t)a->ady << 16;
    k->w[8] = a->aw | a->ah << 12 | (uint32_t)a->aclamp_u << 24 | a->aclamp_v << 25 | a->aflip_u << 26 | a->aflip_v << 27 |
              (uint32_t)(a->awrap_s != 0) << 28 | (uint32_t)(a->awrap_t != 0) << 29 | (uint32_t)a->awflip_s << 30 |
              (uint32_t)a->awflip_t << 31;
}

// The tile's image (and its alpha tile's alpha) as PVR texels in texels[]; returns their format
static uint8_t decode_tile(const struct rdp_tile *t, const struct tile_geom *gm, const struct alpha_src *a, struct tex_fx fx,
                           uint32_t tlut_mode) {
    uint32_t iw = gm->iw, ih = gm->ih, pw = gm->pw, ph = gm->ph, ks = gm->ks, kt = gm->kt, ps = gm->ps, pt = gm->pt;
    uint32_t wrap_s = gm->wrap_s, wrap_t = gm->wrap_t;
    uint8_t wflip_s = gm->wflip_s, wflip_t = gm->wflip_t;
    const struct rdp_tile *at = a->at;
    // 5551 alpha is already 0 or 0xFF, so only a reference of 0 changes it
    uint32_t inv = (fx.invert & 1 ? 0xFF000000u : 0) | (fx.invert & 2 ? 0x00FF0000u : 0) | (fx.invert & 4 ? 0x0000FF00u : 0);
    if (!at && fx.alpha_ref != 0 && !inv && !wrap_s && !wrap_t && t->siz == 2 && t->fmt != 3 && t->fmt != 4 && !(tlut_mode && t->fmt == 2) && ks == 1 && kt == 1)
        return decode_rgba16(t, iw, ih, pw, ph);
    // A folded axis repeats every mask width (twice that mirrored): past the first period, copy
    uint32_t per_s = wrap_s && ks == 1 && !at ? wrap_s << wflip_s : 0, per_t = wrap_t && kt == 1 && !at ? wrap_t << wflip_t : 0;
    uint32_t xdec = per_s && per_s < iw ? per_s : iw;  // columns decoded; the rest of the row copies
    for (uint32_t y = 0; y < ph; y++) {
        uint32_t *row = rgba + y * pw;
        if ((per_t && y >= per_t && y < ih) || y >= ih) {  // padding repeats the last row
            __builtin_memcpy(row, rgba + (y >= ih ? ih - 1 : y - per_t) * pw, pw * 4);
            continue;
        }
        uint32_t sy = pt + y * kt, ty = wrap_t ? fold((int32_t)sy, wrap_t, 0, wflip_t) : sy;
        for (uint32_t x = 0; x < xdec; x++) {
            uint32_t sx = ps + x * ks, tx = wrap_s ? fold((int32_t)sx, wrap_s, 0, wflip_s) : sx;
            uint32_t c = texel(t, tx, ty, tlut_mode);
            if (at) {
                uint32_t ax = fold((int32_t)sx + a->adx, a->aw, a->aclamp_u, a->aflip_u);
                uint32_t ay = fold((int32_t)sy + a->ady, a->ah, a->aclamp_v, a->aflip_v);
                if (a->awrap_s) ax = fold((int32_t)ax, a->awrap_s, 0, a->awflip_s);
                if (a->awrap_t) ay = fold((int32_t)ay, a->awrap_t, 0, a->awflip_t);
                c = (c & ~0xFFu) | (texel(at, ax, ay, tlut_mode) & 0xFF);
            }
            row[x] = c;
        }
        for (uint32_t x = xdec; x < pw; x++) row[x] = row[x < iw ? x - per_s : iw - 1];
    }
    if (fx.alpha_ref >= 0 || inv)
        for (uint32_t i = 0; i < pw * ph; i++) {
            uint32_t c = rgba[i];
            if (fx.alpha_ref >= 0) c = (c & ~0xFFu) | ((int)(c & 0xFF) >= fx.alpha_ref ? 0xFF : 0);
            rgba[i] = c ^ inv;
        }
    uint8_t fmt = choose_fmt(pw * ph);
    for (uint32_t i = 0; i < pw * ph; i++) texels[i] = to16(rgba[i], fmt);
    return fmt;
}

int gfx_tex_bind(int tilenum, int alpha_tile, struct tex_fx fx, const struct tex_dec *want, struct tex_bind *b) {
    const struct rdp_tile *t = &g.tile[tilenum & 7];
    uint32_t tlut_mode = (g.rdp_omh >> OMH_TLUT_SHIFT) & 3;
    struct tile_geom gm;
    struct alpha_src a;
    tile_geometry(t, want, b, &gm);
    alpha_source(t, alpha_tile, &a);
    struct tex_key k;
    tex_key_of(t, &gm, &a, b, fx, tlut_mode, &k);
    gfx_tex_lookups++;
    if (g.tmem_gen == last_gen && last_slot >= 0) {
        k.w[3] = last_key.w[3], k.w[4] = last_key.w[4], k.w[5] = last_key.w[5], k.w[9] = last_key.w[9];
        if (key_eq(&k, &last_key)) {
            slot[last_slot].used = tex_frame;
            b->id = (uint16_t)(last_slot + 1);
            return 1;
        }
    }
    k.w[3] = tile_spans(t, gm.ih_tmem, tlut_mode, &k.w[4], &k.w[5]);
    k.w[9] = 0;
    if (a.at) {
        uint32_t upper, pal;
        k.w[9] = tile_spans(a.at, a.ah, tlut_mode, &upper, &pal);
        k.w[9] = (k.w[9] * 0x01000193u ^ upper) * 0x01000193u ^ pal;
    }

    // Slots are never emptied, so a key always sits before the first empty slot of its window
    uint32_t home = (k.w[3] ^ k.w[0] * 31 ^ k.w[4] ^ k.w[5] * 7 ^ k.w[9] * 13) % TEX_SLOTS, s = home, lru = home;
    for (int probe = 0; probe < TEX_PROBES; probe++, s = (s + 1) % TEX_SLOTS) {
        if (!slot[s].live) break;
        if (key_eq(&slot[s].k, &k)) goto hit;
        if (slot[s].used < slot[lru].used) lru = s;
    }
    if (slot[s].live) {
        s = lru;
        if (slot[s].used + TEX_KEEP_FRAMES > tex_frame) gfx_tex_forced++;  // still drawn: this frame shows it wrong
    }
    slot[s].k = k;
    slot[s].w = (uint16_t)gm.pw, slot[s].h = (uint16_t)gm.ph;
    slot[s].live = 1;
    gfx_tmem_materialize();
    slot[s].fmt = decode_tile(t, &gm, &a, fx, tlut_mode);
    gfx_sink->tex_upload((uint16_t)(s + 1), (int)gm.pw, (int)gm.ph, slot[s].fmt, texels);
    gfx_tex_uploads++;
hit:
    slot[s].used = tex_frame;
    last_key = k;
    last_gen = g.tmem_gen;
    last_slot = (int)s;
    b->id = (uint16_t)(s + 1);
    return 1;
}

void gfx_tex_reset(void) {
    __builtin_memset(slot, 0, sizeof slot);
    last_slot = -1;
    last_gen = 0xFFFFFFFFu;
    tex_frame = TEX_KEEP_FRAMES;
}

void gfx_tex_frame(void) { tex_frame++; }
