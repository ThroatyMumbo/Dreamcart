// Combiner and blender onto the PVR: each channel as t*TEX + a*TEX_ALPHA + c, evaluated per vertex.
#include "gfx.h"
#include "gfx_cc.h"
#include "gfx_sink.h"

#define CYC_SHIFT 20
#define OM_FORCE_BL 0x4000u
#define OM_AA_EN 0x8u
#define OM_CVG_X_ALPHA 0x1000u
#define OM_ALPHA_CVG_SEL 0x2000u
#define OM_ZMODE_SHIFT 10
#define OM_Z_CMP 0x10u
#define OM_Z_UPD 0x20u
#define OM_AC_MASK 0x3u

struct lin { float t, a, c; };

struct cc_in { const uint8_t *shade; int cycle; struct lin comb[4]; };

static const float k255 = 1.0f / 255.0f;

static float byte_of(uint32_t c, int ch) { return (float)((c >> (24 - 8 * ch)) & 0xFF) * k255; }
static struct lin K(float c) { struct lin l = { 0, 0, c }; return l; }
static int has_tex(struct lin l) { return l.t != 0.0f || l.a != 0.0f; }

static struct lin texel(int ch) { struct lin l = { ch < 3 ? 1.0f : 0.0f, ch < 3 ? 0.0f : 1.0f, 0 }; return l; }
static struct lin texel_a(void) { struct lin l = { 0, 1.0f, 0 }; return l; }

static struct lin common_in(const struct cc_in *e, int idx, int ch) {
    switch (idx) {
    case 0: return e->cycle ? e->comb[ch] : K(0);
    case 1: case 2: return texel(ch);
    case 3: return K(byte_of(g.prim, ch));
    case 4: return K((float)e->shade[ch] * k255);
    case 5: return K(byte_of(g.env, ch));
    }
    return K(0);
}

// SETKEYR: center_r 15:8, scale_r 7:0; SETKEYGB w1: center_g 31:24, scale_g 23:16, center_b 15:8, scale_b 7:0
static float key_center(int ch) { return (float)(ch == 0 ? (g.keyr >> 8) & 0xFF : ch == 1 ? g.keygb >> 24 : (g.keygb >> 8) & 0xFF) * k255; }
static float key_scale(int ch) { return (float)(ch == 0 ? g.keyr & 0xFF : ch == 1 ? (g.keygb >> 16) & 0xFF : g.keygb & 0xFF) * k255; }

static struct lin rgb_a(const struct cc_in *e, int i, int ch) { return i == 6 ? K(1) : i == 7 ? K(0.5f) : i < 6 ? common_in(e, i, ch) : K(0); }
static struct lin rgb_b(const struct cc_in *e, int i, int ch) {
    if (i == 6) return K(key_center(ch));
    if (i == 7) return K((float)((g.convert_lo >> 9) & 0x1FF) * k255);
    return i < 6 ? common_in(e, i, ch) : K(0);
}
static struct lin rgb_c(const struct cc_in *e, int i, int ch) {
    switch (i) {
    case 6: return K(key_scale(ch));
    case 7: return e->cycle ? e->comb[3] : K(0);
    case 8: case 9: return texel_a();
    case 10: return K(byte_of(g.prim, 3));
    case 11: return K((float)e->shade[3] * k255);
    case 12: return K(byte_of(g.env, 3));
    case 13: return K(0);
    case 14: return K((float)(g.prim_lod & 0xFF) * k255);
    case 15: return K((float)(g.convert_lo & 0x1FF) * k255);
    }
    return i < 6 ? common_in(e, i, ch) : K(0);
}
static struct lin rgb_d(const struct cc_in *e, int i, int ch) { return i == 6 ? K(1) : i == 7 ? K(0) : common_in(e, i, ch); }
static struct lin alp_abd(const struct cc_in *e, int i) { return i == 6 ? K(1) : i == 7 ? K(0) : common_in(e, i, 3); }
static struct lin alp_c(const struct cc_in *e, int i) { return i == 0 ? K(0) : i == 6 ? K((float)(g.prim_lod & 0xFF) * k255) : i == 7 ? K(0) : common_in(e, i, 3); }

static struct lin sub(struct lin x, struct lin y) { struct lin r = { x.t - y.t, x.a - y.a, x.c - y.c }; return r; }
static struct lin add(struct lin x, struct lin y) { struct lin r = { x.t + y.t, x.a + y.a, x.c + y.c }; return r; }

static int approx;
static struct lin mul(struct lin x, struct lin y) {
    if (has_tex(x) && has_tex(y)) {
        approx = 1;
        y = K(y.t + y.a + y.c);  // the texel taken as white: the PVR has no TEX*TEX
    }
    if (has_tex(y)) { struct lin t = x; x = y; y = t; }
    struct lin r = { x.t * y.c, x.a * y.c, x.c * y.c };
    return r;
}

struct cyc { uint8_t a[4], b[4], c[4], d[4]; };  // rgb in [0..2], alpha in [3]

static void decode(struct cyc c[2]) {
    uint32_t h = g.cc_hi, l = g.cc_lo;
    c[0].a[0] = (h >> 20) & 0xF, c[0].c[0] = (h >> 15) & 0x1F, c[0].a[3] = (h >> 12) & 7, c[0].c[3] = (h >> 9) & 7;
    c[1].a[0] = (h >> 5) & 0xF, c[1].c[0] = h & 0x1F;
    c[0].b[0] = (l >> 28) & 0xF, c[1].b[0] = (l >> 24) & 0xF, c[1].a[3] = (l >> 21) & 7, c[1].c[3] = (l >> 18) & 7;
    c[0].d[0] = (l >> 15) & 7, c[0].b[3] = (l >> 12) & 7, c[0].d[3] = (l >> 9) & 7;
    c[1].d[0] = (l >> 6) & 7, c[1].b[3] = (l >> 3) & 7, c[1].d[3] = l & 7;
}

// (a - b) * c + d for one cycle; the rgb lerp by texel alpha is the PVR's DECAL, kept as decal = 1 with c = d
static void run_cycle(struct cc_in *e, const struct cyc *y, struct lin out[4], int *decal) {
    *decal = 0;
    for (int ch = 0; ch < 3; ch++) {
        struct lin a = rgb_a(e, y->a[0], ch), b = rgb_b(e, y->b[0], ch), c = rgb_c(e, y->c[0], ch), d = rgb_d(e, y->d[0], ch);
        if ((y->c[0] == 8 || y->c[0] == 9) && (y->a[0] == 1 || y->a[0] == 2) && y->b[0] == y->d[0] && !has_tex(d)) {
            *decal = 1;
            out[ch] = d;
            continue;
        }
        out[ch] = add(mul(sub(a, b), c), d);
    }
    struct lin a = alp_abd(e, y->a[3]), b = alp_abd(e, y->b[3]), c = alp_c(e, y->c[3]), d = alp_abd(e, y->d[3]);
    out[3] = add(mul(sub(a, b), c), d);
}

static int passes_rgb(const struct cyc *y) { return (y->c[0] >= 16 || y->a[0] == y->b[0]) && y->d[0] == 0; }

static void evaluate(const uint8_t shade[4], struct lin out[4], int *decal) {
    struct cyc c[2];
    decode(c);
    struct cc_in e;
    __builtin_memset(&e, 0, sizeof e);
    e.shade = shade;
    uint32_t cyc = (g.rdp_omh >> CYC_SHIFT) & 3;
    if (cyc == 1) {
        int d0, d1;
        run_cycle(&e, &c[0], e.comb, &d0);
        e.cycle = 1;
        run_cycle(&e, &c[1], out, &d1);
        *decal = d1 || (d0 && passes_rgb(&c[1]));
        if (d0 && !passes_rgb(&c[1])) approx = 1;
        if (d0 && passes_rgb(&c[1])) for (int ch = 0; ch < 3; ch++) out[ch] = e.comb[ch];
    } else {
        run_cycle(&e, &c[1], out, decal);  // 1-cycle mode runs the second cycle's fields (angrylion combiner_1cycle)
    }
}

static void blend_fields(uint32_t oml, int cycle, int *p, int *a, int *m, int *b) {
    int sh = cycle ? 0 : 2;
    *p = (oml >> (28 + sh)) & 3, *a = (oml >> (24 + sh)) & 3, *m = (oml >> (20 + sh)) & 3, *b = (oml >> (16 + sh)) & 3;
}

static float clampf(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static uint32_t pack(float r, float g_, float b, float a) {
    return (uint32_t)(clampf(a) * 255.0f + 0.5f) << 24 | (uint32_t)(clampf(r) * 255.0f + 0.5f) << 16
           | (uint32_t)(clampf(g_) * 255.0f + 0.5f) << 8 | (uint32_t)(clampf(b) * 255.0f + 0.5f);
}

static int uses_texel(const struct cyc *y) {
    int t = 0;
    for (int k = 0; k < 4; k += 3) {
        t |= (y->a[k] >= 1 && y->a[k] <= 2) || (y->b[k] >= 1 && y->b[k] <= 2) || (y->d[k] >= 1 && y->d[k] <= 2);
        t |= k ? (y->c[k] >= 1 && y->c[k] <= 2) : ((y->c[k] >= 1 && y->c[k] <= 2) || y->c[k] == 8 || y->c[k] == 9);
    }
    return t;
}

// The tiles a cycle reads color and alpha from, as bits: TEXEL0 is tile + t0, TEXEL1 tile + (t0 ^ 1)
static void texel_reads(const struct cyc *y, int t0, int *rgb, int *alpha) {
    for (int idx = 1; idx <= 2; idx++) {
        int bit = 1 << ((idx - 1) ^ t0);
        if (y->a[0] == idx || y->b[0] == idx || y->c[0] == idx || y->d[0] == idx) *rgb |= bit;
        if (y->a[3] == idx || y->b[3] == idx || y->c[3] == idx || y->d[3] == idx || y->c[0] == idx + 7) *alpha |= bit;
    }
}

// Tile offsets for the PVR texture's color and alpha: TEXEL1 alone is tile + 1; color from one texel and alpha from
// the other merges them (Rayman 2's logo masks). The second cycle sees the two swapped
void gfx_cc_texel_tiles(int *rgb_off, int *alpha_off) {
    struct cyc c[2];
    decode(c);
    uint32_t cyc = (g.rdp_omh >> CYC_SHIFT) & 3;
    int rgb = 0, alpha = 0;
    *rgb_off = *alpha_off = 0;
    if (cyc == 1) texel_reads(&c[0], 0, &rgb, &alpha), texel_reads(&c[1], 1, &rgb, &alpha);
    else if (cyc == 0) texel_reads(&c[1], 0, &rgb, &alpha);
    else return;
    if ((rgb == 1 && alpha == 2) || (rgb == 2 && alpha == 1)) *rgb_off = rgb >> 1, *alpha_off = alpha >> 1;
    else if ((rgb | alpha) == 2) *rgb_off = *alpha_off = 1;
}

static float field(const struct lin *l, int f) { return f == 0 ? l->t : f == 1 ? l->a : l->c; }
static float absf(float v) { return v < 0 ? -v : v; }

// The exact evaluator at five shade points: three give the affine coefficients, two check there are no products
// k: [r, g, b, a][texel term, texel-alpha term, constant][1, shade_ch, shade_a], for the build only
static void fit_affine(struct cc_plan *p, float kk[4][3][3]) {
    static const uint8_t probe[5][2] = { { 0, 0 }, { 255, 0 }, { 0, 255 }, { 255, 255 }, { 128, 128 } };
    struct lin r[5][4];
    int decal, saved = approx;
    for (int i = 0; i < 5; i++) {
        uint8_t s[4] = { probe[i][0], probe[i][0], probe[i][0], probe[i][1] };
        evaluate(s, r[i], &decal);
    }
    approx = saved;
    p->affine = 1;
    for (int ch = 0; ch < 4; ch++)
        for (int f = 0; f < 3; f++) {
            float v0 = field(&r[0][ch], f), v1 = field(&r[1][ch], f) - v0, v2 = field(&r[2][ch], f) - v0;
            float mid = v0 + (128.0f / 255.0f) * (v1 + v2);
            if (absf(field(&r[3][ch], f) - (v0 + v1 + v2)) > 1e-4f || absf(field(&r[4][ch], f) - mid) > 1e-4f) p->affine = 0;
            kk[ch][f][0] = v0, kk[ch][f][1] = v1, kk[ch][f][2] = v2;
        }
    p->tex_invert = 0;
    for (int ch = 0; ch < 3; ch++)
        if (p->affine && p->textured && !p->decal && kk[ch][0][0] < 0.0f && kk[ch][0][1] == 0.0f && kk[ch][0][2] == 0.0f)
            p->tex_invert |= (uint8_t)(1u << ch);
    // gfx_cc_vertex's selection, folded: base rgb, base alpha, offset rgb; [1, shade byte, shade alpha byte]
    for (int i = 0; i < 7; i++) {
        int ch = i < 4 ? i : i - 4;
        const float (*k)[3] = kk[ch];
        float inv = ch < 3 && (p->tex_invert >> ch & 1) ? 1.0f : 0.0f;
        float s[3] = { 0, 0, 0 };
        for (int j = 0; j < 3; j++) {
            if (i == 3) s[j] = !p->textured ? k[2][j] + k[1][j] : p->alpha_tex ? k[1][j] : k[2][j];
            else if (i < 3) s[j] = !p->textured ? k[2][j] + k[0][j] + k[1][j] : p->decal ? k[2][j] : k[0][j] * (1.0f - 2.0f * inv);
            else s[j] = p->textured && !p->decal ? k[2][j] + k[0][j] * inv : 0.0f;
        }
        if (i == 3 && p->blend_fog_alpha) s[0] = byte_of(g.fog, 3), s[1] = s[2] = 0;
        p->out[i][0] = s[0] * 255.0f, p->out[i][1] = s[1], p->out[i][2] = s[2];
    }
    p->no_offset = 1;
    for (int i = 4; i < 7; i++)
        for (int j = 0; j < 3; j++) p->no_offset &= p->out[i][j] == 0.0f;
}

static void plan_build(struct cc_plan *p, int texture_on, int rect);

// Recent plans, 64 sets x 4 ways: the same combiner, modes and colors give the same plan, so it is built once
#define PLAN_SETS 64
#define PLAN_WAYS 4
#define PLAN_KEY_WORDS 12
static struct { uint32_t key[PLAN_KEY_WORDS]; int live; struct cc_plan plan; } plans[PLAN_SETS][PLAN_WAYS];
static uint8_t plan_next[PLAN_SETS];  // the way the set's next build replaces, round robin

void gfx_cc_plan(struct cc_plan *p, int texture_on, int rect) {
    uint32_t key[PLAN_KEY_WORDS] = { g.cc_hi, g.cc_lo, g.rdp_omh, g.rdp_oml, g.prim, g.env, g.prim_lod, g.keygb, g.keyr,
                                     g.convert_lo, g.fog, (uint32_t)texture_on | (uint32_t)rect << 1 };
    uint32_t h = 0;
    for (int i = 0; i < PLAN_KEY_WORDS; i++) h = (h ^ key[i]) * 0x9E3779B1u;
    unsigned s = h >> 26;  // the multiply's top bits: every key bit reaches them
    for (int w = 0; w < PLAN_WAYS; w++) {
        int same = plans[s][w].live;
        for (int i = 0; same && i < PLAN_KEY_WORDS; i++) same = plans[s][w].key[i] == key[i];
        if (same) {
            *p = plans[s][w].plan;
            return;
        }
    }
    plan_build(p, texture_on, rect);
    int w = plan_next[s];
    __builtin_memcpy(plans[s][w].key, key, sizeof key);
    plans[s][w].plan = *p;
    plans[s][w].live = 1;
    plan_next[s] = (uint8_t)((w + 1) % PLAN_WAYS);
}

static void plan_build(struct cc_plan *p, int texture_on, int rect) {
    static const uint8_t probe[4] = { 0x80, 0x80, 0x80, 0x80 };
    struct lin o[4];
    struct cyc cy[2];
    int decal;
    approx = 0;
    evaluate(probe, o, &decal);
    decode(cy);
    uint32_t oml = g.rdp_oml, cyc = (g.rdp_omh >> CYC_SHIFT) & 3;
    __builtin_memset(p, 0, sizeof *p);
    p->textured = texture_on && (uses_texel(&cy[1]) || (cyc == 1 && uses_texel(&cy[0])));
    p->decal = decal;
    p->rgb_alpha = !decal && (o[0].a != 0.0f || o[1].a != 0.0f || o[2].a != 0.0f);
    p->alpha_tex = o[3].a != 0.0f;
    p->alpha_const = o[3].c != 0.0f;
    if (p->rgb_alpha) approx = 1;
    if (p->alpha_tex && p->alpha_const) approx = 1;
    p->env = !p->textured ? TXR_MODULATE : decal ? TXR_DECAL : TXR_MODULATEALPHA;
    p->ignore_tex_alpha = p->textured && !decal && !p->alpha_tex;
    p->offset = !decal && (o[0].c != 0.0f || o[1].c != 0.0f || o[2].c != 0.0f) && p->textured;

    int bp, ba, bm, bb;
    if (cyc == 1) {
        blend_fields(oml, 0, &bp, &ba, &bm, &bb);
        p->fog = bp == 3 && ba == 2 && bm == 0 && bb == 0;  // FOG_RGB * SHADE_A + IN * (1 - A)
        blend_fields(oml, 1, &bp, &ba, &bm, &bb);
    } else {
        blend_fields(oml, 0, &bp, &ba, &bm, &bb);
    }
    p->list = GL_OP, p->src = BL_ONE, p->dst = BL_ZERO;
    if ((oml & OM_FORCE_BL) && bp == 0 && bm == 1 && (bb == 0 || bb == 2)) {
        p->list = GL_TR;
        p->src = ba == 3 ? BL_ZERO : BL_SRCALPHA;
        p->dst = bb == 2 ? BL_ONE : BL_INVSRCALPHA;
        p->blend_fog_alpha = ba == 1;
    } else if (((oml & OM_CVG_X_ALPHA) && (oml & OM_ALPHA_CVG_SEL)) || (oml & OM_AC_MASK) == 1) {
        p->list = GL_PT;
    }
    p->zmode = (oml >> OM_ZMODE_SHIFT) & 3;
    // With antialiasing the blender skips zero-coverage pixels, color and depth: coverage from texel alpha cuts holes
    p->alpha_cvg = (oml & OM_AA_EN) && (oml & (OM_CVG_X_ALPHA | OM_ALPHA_CVG_SEL)) && p->alpha_tex;
    if (p->list == GL_TR && p->alpha_cvg) {
        static const uint8_t full_a[4] = { 0x80, 0x80, 0x80, 0xFF };
        struct lin o2[4];
        int d2, keep = approx;
        evaluate(full_a, o2, &d2);
        approx = keep;
        if (o2[3].a != o[3].a) p->alpha_cvg = 0;  // weighted by shade alpha, a per-vertex fade: keeps writing depth
        else if (o[3].a >= 0.99f && o[3].t == 0.0f && o[3].c == 0.0f) p->list = GL_PT, p->alpha_cvg = 0;  // a cutout
    }
    p->approx = approx;
    (void)rect;
    float k[4][3][3];
    fit_affine(p, k);
    int texel_alpha = p->affine && k[3][1][0] == 1.0f;
    for (int f = 0; f < 3; f++)
        for (int j = 0; j < 3; j++) texel_alpha &= (f == 1 && j == 0) || k[3][f][j] == 0.0f;
    p->alpha_test = p->list == GL_PT && p->textured && cyc < 2 && (oml & OM_AC_MASK) == 1 && texel_alpha &&
                    !((oml & OM_CVG_X_ALPHA) && (oml & OM_ALPHA_CVG_SEL));
}

// Rounded once and clamped in integers: the same bytes as clamping the float first, without two float compares
static uint32_t byte_out(float v) {
    int32_t i = (int32_t)(v + 0.5f);
    return i < 0 ? 0 : i > 255 ? 255 : (uint32_t)i;
}

GFX_COLD static void cc_vertex_exact(const struct cc_plan *p, const uint8_t shade[4], uint32_t *argb, uint32_t *oargb);

GFX_HOT(05) void gfx_cc_vertex(const struct cc_plan *p, const uint8_t shade[4], uint32_t *argb, uint32_t *oargb) {
    if (p->affine) {
        const float (*o)[3] = p->out;
        float s0 = (float)shade[0], s1 = (float)shade[1], s2 = (float)shade[2], sa = (float)shade[3];
        uint32_t r = byte_out(o[0][0] + o[0][1] * s0 + o[0][2] * sa), gg = byte_out(o[1][0] + o[1][1] * s1 + o[1][2] * sa);
        uint32_t b = byte_out(o[2][0] + o[2][1] * s2 + o[2][2] * sa), a = byte_out(o[3][0] + o[3][1] * sa + o[3][2] * sa);
        *argb = a << 24 | r << 16 | gg << 8 | b;
        if (p->no_offset) {
            *oargb = p->fog ? (uint32_t)shade[3] << 24 : 0;
            return;
        }
        uint32_t orr = byte_out(o[4][0] + o[4][1] * s0 + o[4][2] * sa), og = byte_out(o[5][0] + o[5][1] * s1 + o[5][2] * sa);
        uint32_t ob = byte_out(o[6][0] + o[6][1] * s2 + o[6][2] * sa);
        *oargb = (p->fog ? (uint32_t)shade[3] : 0) << 24 | orr << 16 | og << 8 | ob;
        return;
    }
    cc_vertex_exact(p, shade, argb, oargb);
}

// The symbolic evaluator per vertex, for combiners that aren't affine in shade
static void cc_vertex_exact(const struct cc_plan *p, const uint8_t shade[4], uint32_t *argb, uint32_t *oargb) {
    struct lin o[4];
    int decal;
    evaluate(shade, o, &decal);
    float fog = p->fog ? (float)shade[3] * k255 : 0.0f;
    float alpha = p->blend_fog_alpha ? byte_of(g.fog, 3) : p->alpha_tex ? o[3].a : o[3].c;
    if (!p->textured) {
        *argb = pack(o[0].c + o[0].t + o[0].a, o[1].c + o[1].t + o[1].a, o[2].c + o[2].t + o[2].a, o[3].c + o[3].a);
        *oargb = pack(0, 0, 0, fog);
        return;
    }
    if (decal) {
        *argb = pack(o[0].c, o[1].c, o[2].c, alpha);
        *oargb = pack(0, 0, 0, fog);
        return;
    }
    *argb = pack(o[0].t, o[1].t, o[2].t, alpha);
    *oargb = pack(o[0].c, o[1].c, o[2].c, fog);
}
