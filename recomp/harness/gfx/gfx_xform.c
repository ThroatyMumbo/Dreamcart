// Fast3D matrices and vertices: dma_MTX, imm_POPMTX, dma_VTX and the lighting overlay of rsp/fast3d.s.
#include "gfx.h"
#include "f3d.h"
#include "os.h"

#define DMEM_NUMLIGHTS 0x12Cu
#define DMEM_LIGHTS 0x1A0u  // light n's color at 0x1B0 + 0x20 * (n + 1), its transformed dir at +0x10

static float fix_mtx(uint32_t i_word, uint32_t f_word, int hi) {
    int16_t i = (int16_t)(hi ? i_word >> 16 : i_word);
    uint16_t f = (uint16_t)(hi ? f_word >> 16 : f_word);
    return (float)i + (float)f * (1.0f / 65536.0f);
}

#include "gfx_sh4.h"

void gfx_update_mp(void) {
    mul44(g.mp, g.mv, g.proj);
}

// An s15.16 Mtx in DMEM: 16 integer halves, then 16 fraction halves
void gfx_mtx_from_dmem(uint32_t a, float m[4][4]) {
    for (int k = 0; k < 16; k++) {
        int16_t i = (int16_t)dmem16(a + 2u * k);
        uint16_t f = dmem16(a + 0x20u + 2u * k);
        m[k >> 2][k & 3] = (float)i + (float)f * (1.0f / 65536.0f);
    }
}

void gfx_mtx_to_dmem(uint32_t a, const float m[4][4]) {
    for (int k = 0; k < 16; k++) {
        float x = m[k >> 2][k & 3] * 65536.0f;
        x = x < -2147483648.0f ? -2147483648.0f : x > 2147483520.0f ? 2147483520.0f : x;
        int32_t v = (int32_t)x;
        if ((float)v > x) v--;
        g.dmem[(a + 2u * k) & 0xFFF] = (uint8_t)(v >> 24), g.dmem[(a + 2u * k + 1) & 0xFFF] = (uint8_t)(v >> 16);
        g.dmem[(a + 0x20u + 2u * k) & 0xFFF] = (uint8_t)(v >> 8), g.dmem[(a + 0x21u + 2u * k) & 0xFFF] = (uint8_t)v;
    }
}

// G_MW_MATRIX / G_MV_MATRIX_n wrote the MP matrix in DMEM
void gfx_mp_from_dmem(void) {
    gfx_mtx_from_dmem(gfx_uc->mp, g.mp);
}

// The viewport (scale, then translate, s13.2) times the sign vector, and the fog words
void gfx_vp_from_dmem(void) {
    uint32_t vp = gfx_uc->viewport, sg = gfx_uc->vp_sign, fog = gfx_uc->fog;
    for (int c = 0; c < 3; c++) {
        int16_t sign = sg ? (int16_t)dmem16(sg + 2u * c) : c == 1 ? -1 : 1;
        g.dm_vs[c] = (float)((int16_t)dmem16(vp + 2u * c) * sign) * 0.25f;
        g.dm_vt[c] = (float)(int16_t)dmem16(vp + 8u + 2u * c) * 0.25f;
        g.dm_fog[c] = c < 2 || gfx_uc->fog_min ? (int16_t)dmem16(fog + 2u * c) : 255;  // F3DEX2 clamps fog to a byte
    }
}

static inline __attribute__((always_inline)) void read_mtx(uint32_t phys, float m[4][4]) {
    for (int k = 0; k < 16; k += 2) {
        uint32_t iw = gfx_rd32(phys + 2 * k), fw = gfx_rd32(phys + 32 + 2 * k);
        m[k >> 2][k & 3] = fix_mtx(iw, fw, 1);
        m[k >> 2][(k & 3) + 1] = fix_mtx(iw, fw, 0);
    }
}

void gfx_load_mtx(uint32_t phys, uint32_t flags) {
    float m[4][4];
    read_mtx(phys, m);
    float (*dst)[4] = (flags & G_MTX_PROJECTION) ? g.proj : g.mv;
    if (!(flags & G_MTX_PROJECTION) && (flags & G_MTX_PUSH) && g.mv_depth < g.mv_max)
        __builtin_memcpy(g.mv_stack[g.mv_depth++], g.mv, sizeof g.mv);
    if (flags & G_MTX_LOAD) __builtin_memcpy(dst, m, sizeof m);
    else mul44(dst, m, dst);
    gfx_update_mp();
}

void gfx_pop_mtx(void) {
    if (g.mv_depth == 0) return;
    __builtin_memcpy(g.mv, g.mv_stack[--g.mv_depth], sizeof g.mv);
    gfx_update_mp();
}

// f3dex2.s G_MTX_handler: a push DMAs MV to the RDRAM stack with no bound, so only the HLE's copy has a depth limit
void gfx_load_mtx2(uint32_t phys, int proj, int load, int push) {
    float m[4][4];
    read_mtx(phys, m);
    if (push) {
        if (g.mv_depth == MV_STACK_CAP) os_die("rsp gfx: F3DEX2 matrix stack deeper than 16", (uint32_t)g.mv_depth);
        __builtin_memcpy(g.mv_stack[g.mv_depth++], g.mv, sizeof g.mv);
    }
    float (*dst)[4] = proj ? g.proj : g.mv;
    if (load) __builtin_memcpy(dst, m, sizeof m);
    else mul44(dst, m, dst);
}

// G_POPMTX_handler: n matrices back, clamped at the stack base, and MV reloaded from there unless nothing moved
int gfx_pop_mtx2(uint32_t n) {
    int d = n >= (uint32_t)g.mv_depth ? 0 : g.mv_depth - (int)n;
    if (d == g.mv_depth) return 0;
    g.mv_depth = d;
    __builtin_memcpy(g.mv, g.mv_stack[d], sizeof g.mv);
    return 1;
}

// The s8 direction at DMEM a by MV's upper 3x3 (column form); returns its squared length
static inline __attribute__((always_inline)) float dir_by_mv(uint32_t a, float r[3]) {
    float d[3];
    for (int c = 0; c < 3; c++) d[c] = (float)(int8_t)g.dmem[(a + c) & 0xFFF];
    for (int c = 0; c < 3; c++) r[c] = g.mv[c][0] * d[0] + g.mv[c][1] * d[1] + g.mv[c][2] * d[2];
    return r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
}

// init_lights: each light's direction, and both lookats, by MV, normalized to s8
static void init_lights(uint32_t n) {
    for (uint32_t off = n; ; off -= 0x20) {
        uint32_t a = DMEM_LIGHTS + off;
        float r[3], len2 = dir_by_mv(a + 0x18, r);
        float k = len2 > 0.0f ? 127.0f / __builtin_sqrtf(len2) : 0.0f;
        for (int c = 0; c < 3; c++) {
            int q = (int)(r[c] * k);
            g.dmem[(a + 0x20 + c) & 0xFFF] = (uint8_t)(q > 127 ? 127 : q < -127 ? -127 : q);
            g.dmem[(a + 0x24 + c) & 0xFFF] = g.dmem[(a + 0x20 + c) & 0xFFF];
        }
        if (off == 0) break;
    }
}

static int16_t vmulf(int16_t a, int16_t b) { return sat_i16(((int32_t)a * b * 2 + 0x8000) >> 16); }

// The lights as the overlay's loop walks them, read from DMEM once per lit G_VTX
#define MAX_LIGHT_TERMS 8  // 7 lights and lookAt X: numLights 0x100 down to 0x20; the batch pool stays cache-sized
struct light_cache {
    int n;
    int8_t dir[MAX_LIGHT_TERMS][3];
    uint8_t col[MAX_LIGHT_TERMS][3];
    uint8_t amb[3];
    int8_t look[2][3];
};

static void dir_at(int8_t d[3], uint32_t a) { for (int c = 0; c < 3; c++) d[c] = (int8_t)g.dmem[(a + c) & 0xFFF]; }

// Terms run from numLights down to 0x20: the real lights, then lookAt X (color 0x1D0, dir 0x1E0), as fast3d.s adds it
static void lights_prepare(struct light_cache *lc) {
    uint32_t nl = dmem32(DMEM_NUMLIGHTS);
    if (nl & 0x80000000u) {
        nl &= 0xFFF;
        dmem_set32(DMEM_NUMLIGHTS, nl);
        init_lights(nl);
    }
    nl &= 0xFFF;
    if (nl > 0x20u * MAX_LIGHT_TERMS) os_die("rsp gfx: numLights beyond Fast3D's 7 lights", nl);
    for (int c = 0; c < 3; c++) lc->amb[c] = g.dmem[(DMEM_LIGHTS + 0x30 + nl + c) & 0xFFF];
    lc->n = 0;
    for (uint32_t off = nl; off >= 0x20 && lc->n < MAX_LIGHT_TERMS; off -= 0x20, lc->n++) {
        dir_at(lc->dir[lc->n], DMEM_LIGHTS + 0x20 + off);
        for (int c = 0; c < 3; c++) lc->col[lc->n][c] = g.dmem[(DMEM_LIGHTS + 0x10 + off + c) & 0xFFF];
    }
    dir_at(lc->look[0], DMEM_LIGHTS + 0x40);  // lookAtX (0x1D0), transformed dir at 0x1E0
    dir_at(lc->look[1], DMEM_LIGHTS + 0x20);  // lookAtY (0x1B0), transformed dir at 0x1C0
}

// vmulf(n << 8, d << 8) is exactly 2nd for |d| <= 127, so the dot product is one integer sum
static int16_t dot8(const int8_t n[3], const int8_t d[3]) { return sat_i16(2 * (n[0] * d[0] + n[1] * d[1] + n[2] * d[2])); }

// Both overlays' end: the lit RGB from s.15 sums, and the lookat dots for texgen
static inline __attribute__((always_inline)) void lit_out(struct gvtx *v, const int8_t n[3], const struct light_cache *lc,
                                                         int16_t look[2], int texgen, const uint32_t acc[3]) {
    if (texgen) {
        look[0] = dot8(n, lc->look[0]);
        look[1] = dot8(n, lc->look[1]);
    }
    v->r = (uint8_t)(acc[0] >> 7);
    v->g = (uint8_t)(acc[1] >> 7);
    v->b = (uint8_t)(acc[2] >> 7);
}

// The lighting overlay: ambient plus each light's max(0, color x N.L), saturating; never below 0
static void light_vertex(struct gvtx *v, const int8_t n[3], const struct light_cache *lc, int16_t look[2], int texgen) {
    int32_t acc[3] = { lc->amb[0] << 7, lc->amb[1] << 7, lc->amb[2] << 7 };
    for (int i = 0; i < lc->n; i++) {
        int16_t d = dot8(n, lc->dir[i]);
        for (int c = 0; c < 3; c++) {
            int16_t t = vmulf((int16_t)(lc->col[i][c] << 7), d);
            if (t > 0) acc[c] = sat_i16(acc[c] + t);
        }
    }
    lit_out(v, n, lc, look, texgen, (const uint32_t *)acc);
}

// f3dex2.s continue_light_dir_xfrm: lookat X, Y and each light's dir times MV's 3x3, normalized as floor(128 d / |d|)
static void init_lights2(uint32_t n) {
    for (uint32_t i = 0; i < n + 2; i++) {
        uint32_t a = X2_LOOKAT + X2_LIGHT_SIZE * i;
        float r[3], len2 = dir_by_mv(a + 8, r);
        float k = len2 > 0.0f ? 128.0f / __builtin_sqrtf(len2) : 0.0f;
        for (int c = 0; c < 3; c++) {
            float x = r[c] * k;
            int q = (int)x;
            q -= (float)q > x;
            q = q > 127 ? 127 : q < -128 ? -128 : q;
            g.dmem[(a + 0x10 + c) & 0xFFF] = g.dmem[(a + 0x14 + c) & 0xFFF] = (uint8_t)q;
        }
    }
}

// The lights from the top one down, as overlay 2 walks them; ambient is the slot after the last
static void lights_prepare2(struct light_cache *lc) {
    uint32_t nl = g.dmem[X2_NUMLIGHTS], n = nl / X2_LIGHT_SIZE;
    if (nl % X2_LIGHT_SIZE || n == 0 || n > MAX_LIGHT_TERMS - 1) os_die("rsp gfx: F3DEX2 numLights not 1..7 lights", nl);
    if (!g.dmem[X2_LIGHTS_VALID]) {
        g.dmem[X2_LIGHTS_VALID] = 1;
        init_lights2(n);
    }
    for (int c = 0; c < 3; c++) lc->amb[c] = g.dmem[(X2_LIGHTS + X2_LIGHT_SIZE * n + c) & 0xFFF];
    lc->n = (int)n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t a = X2_LIGHTS + X2_LIGHT_SIZE * (n - 1 - i);
        dir_at(lc->dir[i], a + 0x10);
        for (int c = 0; c < 3; c++) lc->col[i][c] = g.dmem[(a + c) & 0xFFF];
    }
    dir_at(lc->look[0], X2_LOOKAT + 0x10);
    dir_at(lc->look[1], X2_LOOKAT + X2_LIGHT_SIZE + 0x10);
}

// vmulu/vmacu: the dot clamped to 0..0x7FFF
static int32_t dot8u(const int8_t n[3], const int8_t d[3]) {
    int32_t s = 2 * (n[0] * d[0] + n[1] * d[1] + n[2] * d[2]);
    return s < 0 ? 0 : s > 0x7FFF ? 0x7FFF : s;
}

// lights_dircoloraccum2: per light pair, vmulf(color, 0x7FFF) plus both color x dot terms, one saturation; lit alpha
// above 128 loses 1. The terms are >= 0 and < 2^31, so 32 bits are exact: past 2^31 after two, it saturates anyway
static void light_vertex2(struct gvtx *v, const int8_t n[3], const struct light_cache *lc, int16_t look[2], int texgen) {
    uint32_t acc[3] = { lc->amb[0] << 7u, lc->amb[1] << 7u, lc->amb[2] << 7u };
    for (int i = 0; i < lc->n; i += 2) {
        int two = i + 1 < lc->n;
        uint32_t d0 = (uint32_t)dot8u(n, lc->dir[i]), d1 = two ? (uint32_t)dot8u(n, lc->dir[i + 1]) : 0;
        for (int c = 0; c < 3; c++) {
            uint32_t a = (acc[c] << 16) - 2 * acc[c] + 0x8000 + (lc->col[i][c] * d0 << 8);
            if (two && a < 0x80000000u) a += lc->col[i + 1][c] * d1 << 8;
            a >>= 16;
            acc[c] = a > 0x7FFF ? 0x7FFF : a;
        }
    }
    lit_out(v, n, lc, look, texgen, acc);
    v->a = (uint8_t)((((v->a << 7) * 0xFFFE + 0x8000) >> 16) >> 7);
}

// lights_texgenmain's G_TEXTURE_GEN_LINEAR; under 2.04's BUG_TEXGEN_LINEAR_CLOBBER_S_T its x * 0x44D3 term reads 0x4000
GFX_COLD static int32_t texgen_linear2(int16_t look) {
    int32_t x = look >> 1;
    int32_t x2 = sat_i16((2 * x * x + 0x8000) >> 16);
    int32_t c = sat_i16((int32_t)((2 * (int64_t)x * 0x7FFF + 0x8000 + 2 * (int64_t)x * 0x6CB3) >> 16));
    int64_t m = gfx_uc->texgen_fixed ? x : 0x4000;
    int64_t acc = ((int64_t)0x4000 << 16) + 2 * m * 0x44D3 + 2 * (int64_t)x2 * c;
    return sat_i16((int32_t)(acc >> 16));
}

// RSP vch/vcl on (component, bound): the 32-bit signed rule, including w < 0
static int clip_le(float c, float w) { return ((c < 0) != (w < 0)) ? (c + w <= 0) : (w < 0); }
static int clip_ge(float c, float w) { return ((c < 0) != (w < 0)) ? (w < 0) : (c - w >= 0); }

// c <= -b as !(c > -b): one fcmp/gt each, exact because ftrv of 16-bit ints through an s15.16 matrix is never NaN
static inline __attribute__((always_inline)) uint32_t le3(float x, float y, float z, float nb) {
    return 7u ^ ((uint32_t)(x > nb) + 2u * (y > nb) + 4u * (z > nb));
}
static inline __attribute__((always_inline)) uint32_t ge3(float x, float y, float z, float b) {
    return 7u ^ ((uint32_t)(b > x) + 2u * (b > y) + 4u * (b > z));
}

static uint16_t clip_code(float x, float y, float z, float w, float ratio) {
    uint16_t code = 0;
    float wr = w * ratio;
    if (w > 0.0f && wr > 0.0f) {  // for a bound b > 0 the rule is c <= -b and c >= b
        uint32_t le = le3(x, y, z, -w), ge = ge3(x, y, z, w);
        if (ratio == 1.0f) {
            uint32_t both = le | ge << 8;  // SH-4 shifts by 8 in one step; the w and ratio bits are this, 4 apart
            return (uint16_t)(both | both << 4);
        }
        uint32_t lr = le3(x, y, z, -wr), gr = ge3(x, y, z, wr);
        return (uint16_t)(lr | gr << 8 | (le | ge << 8) << 4);
    }
    const float p[4] = { x, y, z, w };
    for (int c = 0; c < 3; c++) {
        if (clip_le(p[c], p[3])) code |= (uint16_t)(0x10 << c);
        if (clip_ge(p[c], p[3])) code |= (uint16_t)(0x1000 << c);
        if (clip_le(p[c], wr)) code |= (uint16_t)(0x01 << c);
        if (clip_ge(p[c], wr)) code |= (uint16_t)(0x0100 << c);
    }
    return code;
}

// The state a G_VTX ran under, for finishing its vertices later; one more than the live vertices always has a free one
struct vtx_batch {
    uint32_t geom, ss, ts;
    float vs[3], vt[3];
    int16_t fm, fo, fmin;
    struct light_cache lc;
};
#define NBATCH (GFX_MAX_VTX + 1)
static struct vtx_batch batches[NBATCH];
static uint8_t batch_refs[NBATCH];
const void *const gfx_batches = batches;
const uint32_t gfx_batches_bytes = sizeof batches;

static struct vtx_batch *batch_claim(void) {
    for (int i = 0; i < NBATCH; i++)
        if (!batch_refs[i]) return &batches[i];
    os_die("rsp gfx: no free vertex batch (refcount leak)", 0);
}

GFX_HOT(01) void gfx_load_vtx(uint32_t phys, uint32_t n, uint32_t v0, uint32_t tex, int16_t clip_ratio) {
    struct vtx_batch *bt = batch_claim();
    for (int c = 0; c < 3; c++) {
        bt->vs[c] = g.vp_scale[c] = g.dm_vs[c];
        bt->vt[c] = g.vp_trans[c] = g.dm_vt[c];
    }
    bt->geom = g.geom;
    if (g.geom & G_LIGHTING) {
        if (gfx_uc->kind == UC_F3DEX2) {
            if (gfx_uc->point_lights && (g.geom & G_LIGHTING_POSITIONAL))
                os_die("rsp gfx: G_LIGHTING_POSITIONAL (F3DEX2 2.08PL point lighting) isn't modeled", g.geom);
            lights_prepare2(&bt->lc);
        } else {
            lights_prepare(&bt->lc);
            if (g.geom & G_TEXTURE_GEN_LINEAR) os_die("rsp gfx: G_TEXTURE_GEN_LINEAR (acos texgen) isn't modeled", g.geom);
        }
    }
    bt->ss = tex >> 16, bt->ts = tex & 0xFFFF;
    bt->fm = g.dm_fog[0], bt->fo = g.dm_fog[1], bt->fmin = g.dm_fog[2];
    uint8_t ortho = g.mp[0][3] == 0.0f && g.mp[1][3] == 0.0f && g.mp[2][3] == 0.0f;
    float ratio = (float)clip_ratio;
    gfx_guard_band(ratio);
    int no_near = gfx_uc->no_near;  // NoN: the near bit marks a vertex behind the eye instead
    uint16_t keep = no_near ? (uint16_t)~CLIP_NEAR : 0xFFFFu, behind = no_near ? CLIP_NEAR : 0;
    uint8_t b = (uint8_t)(bt - batches);
    xmtrx_load(g.mp);
    const uint32_t *src = (const uint32_t *)(0x80000000u + phys + t_rdram_bias);  // the vertex stores may alias the bias
    for (uint32_t i = 0; i < n; i++, src += 4) {
        struct gvtx *v = &g.v[v0 + i];
        uint32_t w0 = src[0], w1 = src[1];
        if (!((uintptr_t)src & 31)) __builtin_prefetch(src + 16);
        register float f0 __asm__("fr0") = (float)(int16_t)(w0 >> 16), f1 __asm__("fr1") = (float)(int16_t)w0;
        register float f2 __asm__("fr2") = (float)(int16_t)(w1 >> 16), f3 __asm__("fr3") = 1.0f;
        FTRV_FV0(f0, f1, f2, f3);
        v->x = f0, v->y = f1, v->z = f2, v->w = f3;
        v->clip = (uint16_t)((clip_code(f0, f1, f2, f3, ratio) & keep) | (f3 > 0.0f ? 0 : behind));
        v->ortho = ortho;
        v->pv_stamp = 0;
        v->raw_st = src[2], v->raw_c = src[3];
        if (v->batch != NO_BATCH) batch_refs[v->batch]--;  // never b: it was claimed with no references
        v->batch = b;
        v->done = 0, v->xy = 0;
    }
    batch_refs[b] += (uint8_t)n;
}

static float sat16f(float x) { return x < -32768.0f ? -32768.0f : x >= 32768.0f ? 32767.99998f : x; }

// Both ucodes' finish starts alike: clip-space p, screen xy and 1/w, the raw s and t, and the load's color word
static inline __attribute__((always_inline)) float vtx_begin(struct gvtx *v, float p[4], int32_t *s, int32_t *t,
                                                            uint32_t *color) {
    p[0] = v->x, p[1] = v->y, p[2] = v->z, p[3] = v->w;
    gfx_vtx_xy_now(v);
    float iw = v->iw;
    uint32_t w2 = v->raw_st, w3 = v->raw_c;
    *s = (int16_t)(w2 >> 16), *t = (int16_t)w2, *color = w3;
    v->r = (uint8_t)(w3 >> 24), v->g = (uint8_t)(w3 >> 16), v->b = (uint8_t)(w3 >> 8), v->a = (uint8_t)w3;
    return iw;
}

// And goes on alike: s and t by the load's scale, then the viewport
static inline __attribute__((always_inline)) void vtx_place(struct gvtx *v, const struct vtx_batch *bt, const float p[4],
                                                           float iw, int32_t s, int32_t t) {
    v->s = (s * (int32_t)bt->ss) >> 16;
    v->t = (t * (int32_t)bt->ts) >> 16;
    v->sx = p[0] * iw * bt->vs[0] + bt->vt[0];
    v->sy = p[1] * iw * bt->vs[1] + bt->vt[1];
    v->sz = p[2] * iw * bt->vs[2] + bt->vt[2];
}

// The rest of fast3d.s's per-vertex work, done when a triangle first needs the vertex, with its load's state
GFX_HOT(02) static void vtx_finish_f3d(struct gvtx *v) {
    if (v->done) return;
    v->done = 1;
    const struct vtx_batch *bt = &batches[v->batch];
    float p[4];
    int32_t s, t;
    uint32_t w3;
    float iw = vtx_begin(v, p, &s, &t, &w3);
    if (bt->geom & G_LIGHTING) {
        int8_t nrm[3] = { (int8_t)(w3 >> 24), (int8_t)(w3 >> 16), (int8_t)(w3 >> 8) };
        int16_t look[2];
        light_vertex(v, nrm, &bt->lc, look, bt->geom & G_TEXTURE_GEN);
        if (bt->geom & G_TEXTURE_GEN) {
            s = (look[0] >> 1) + 0x4000;  // (N.L + 1) / 4 of the scale, as in the lighting overlay
            t = (look[1] >> 1) + 0x4000;
        }
    }
    vtx_place(v, bt, p, iw, s, t);
    if (bt->geom & G_FOG) {
        // fast3d.s, F3DEX: a negative 1/w is replaced by 0x7FFF (vmrg $v30[0]), and vmadh saturates z/w and z/w * fm
        // to 16 bits, so a vertex behind the eye gets no fog rather than all of it (Rayman 2's NoN ocean)
        float zw = sat16f(p[2] * (p[3] > 0.0f ? iw : 32767.0f)), zf = sat16f(zw * (float)bt->fm);
        int32_t f = (int32_t)zf;
        if ((float)f > zf) f--;
        f += bt->fo;
        f = f < -32768 ? -32768 : f > 32767 ? 32767 : f;
        v->a = (uint8_t)(f < 0 ? 0 : f > bt->fmin ? bt->fmin : f);
    }
}

// f3dex2.s vertices_store and overlay 2: its own lighting and texgen; fog is fo + floor(z/w * fm), only z/w saturated,
// then clamped to a byte (vge against 0x7F00 on 0x7F00 + fog)
GFX_HOT(10) static void vtx_finish_ex2(struct gvtx *v) {
    if (v->done) return;
    v->done = 1;
    const struct vtx_batch *bt = &batches[v->batch];
    float p[4];
    int32_t s, t;
    uint32_t w3;
    float iw = vtx_begin(v, p, &s, &t, &w3);
    if (bt->geom & G_LIGHTING) {
        int8_t nrm[3] = { (int8_t)(w3 >> 24), (int8_t)(w3 >> 16), (int8_t)(w3 >> 8) };
        int16_t look[2];
        light_vertex2(v, nrm, &bt->lc, look, bt->geom & G_TEXTURE_GEN);
        if (bt->geom & G_TEXTURE_GEN) {
            s = (look[0] >> 1) + 0x4000;
            t = (look[1] >> 1) + 0x4000;
            if (bt->geom & G_TEXTURE_GEN_LINEAR) s = texgen_linear2(look[0]), t = texgen_linear2(look[1]);
        }
    }
    vtx_place(v, bt, p, iw, s, t);
    if (bt->geom & G_FOG) {
        float zw = sat16f(p[2] * (p[3] > 0.0f ? iw : 32767.0f)), zf = zw * (float)bt->fm;
        int32_t f = (int32_t)zf;
        if ((float)f > zf) f--;
        f += bt->fo;
        v->a = (uint8_t)(f < 0 ? 0 : f > 255 ? 255 : f);
    }
}

void (*gfx_vtx_finish)(struct gvtx *v) = vtx_finish_f3d;

GFX_HOT(03) void gfx_vtx_xy_call(struct gvtx *v) { gfx_vtx_xy_now(v); }

void gfx_modify_vtx(uint32_t off, uint32_t val) {
    uint32_t n = off / 40, where = off % 40;
    if (n >= gfx_uc->nvtx) os_die("rsp gfx: G_MW_POINTS past the vertex buffer", off);
    struct gvtx *v = &g.v[n];
    gfx_vtx_finish(v);
    switch (where) {
    case G_MWO_POINT_RGBA:
        v->r = (uint8_t)(val >> 24), v->g = (uint8_t)(val >> 16), v->b = (uint8_t)(val >> 8), v->a = (uint8_t)val;
        break;
    case G_MWO_POINT_ST: v->s = (int16_t)(val >> 16), v->t = (int16_t)val; break;
    case G_MWO_POINT_XYSCREEN: v->sx = (float)(int16_t)(val >> 16) * 0.25f, v->sy = (float)(int16_t)val * 0.25f; break;
    case G_MWO_POINT_ZSCREEN:  // the RSP's s15.16 screen z; sz is a quarter of it, as the viewport's z is scaled
        if (gfx_uc->kind != UC_F3DEX2) os_die("rsp gfx: G_MW_POINTS offset other than RGBA, ST or XYSCREEN", off);
        v->sz = (float)(int32_t)val * (0.25f / 65536.0f);
        break;
    default: os_die("rsp gfx: G_MW_POINTS offset other than RGBA, ST or XYSCREEN", off);
    }
    v->pv_stamp = 0;
}

void gfx_vtx_reset(void) {
    gfx_vtx_finish = gfx_uc->kind == UC_F3DEX2 ? vtx_finish_ex2 : vtx_finish_f3d;
    __builtin_memset(batch_refs, 0, sizeof batch_refs);
    for (int i = 0; i < GFX_MAX_VTX; i++) g.v[i].batch = NO_BATCH, g.v[i].done = g.v[i].xy = 1;
}

int gfx_cull_dl(uint32_t vfirst, uint32_t vend) {
    uint16_t all = gfx_uc->cull_mask;
    if (vfirst >= gfx_uc->nvtx || vend > gfx_uc->nvtx || vend <= vfirst) return 0;
    for (uint32_t i = vfirst; i < vend; i++) all &= g.v[i].clip;
    return all != 0;
}
