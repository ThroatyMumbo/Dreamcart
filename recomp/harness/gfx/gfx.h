#ifndef GFX_H
#define GFX_H

#include "htypes.h"
#include "gfx_sink.h"

#define RDRAM_SIZE RDRAM_BYTES
// The per-vertex/per-triangle path, laid out together in this order (link with --sort-section=name): the SH-4's
// 8 KB direct-mapped I-cache otherwise evicts one hot function with another
#define GFX_HOT(n) __attribute__((section(".text.gfxhot." #n)))
#define GFX_COLD __attribute__((cold, noinline))

#define DMEM_STATE 0x110u  // fast3d.s $29: geometry 0x114, othermode 0x118, texture 0x120, numLights 0x12C
#define GFX_MAX_VTX 32
#define MV_STACK_CAP 16

// The supported ucodes differ in these DMEM addresses and in their command encodings (rsp_gfx.c, rsp_gbi2.c)
enum { UC_F3D, UC_F3DEX, UC_F3DLX, UC_F3DEX2, UC_S2DEX2 };
struct gfx_ucode {
    const char *version;
    uint16_t version_off, viewport, mp, dl_depth, cull_mask;
    uint16_t texture;     // G_TEXTURE's two words: tile at w0 bits 8..10, the s/t scale in w1
    uint16_t clip_ratio;  // the guard band's ratio, a positive halfword
    uint16_t vp_sign;     // the viewport scale's sign vector; 0: F3DEX2's fixed (1, -1, 1)
    uint16_t fog;         // multiplier, offset, and with fog_min a third halfword capping the result
    uint8_t kind, nvtx, fog_min;
    uint8_t f3dex12;   // F3DEX 1.2x: BRANCH_Z and LOAD_UCODE are real; texrects take RDPHALF_1 + RDPHALF_2
    uint8_t flat_rgb;  // flat shading takes only RGB from the first vertex; alpha stays per vertex
    uint8_t special1_mvp;  // F3DEX2 2.04H: G_SPECIAL_1 recomputes MP; other F3DEX2s treat it as a no-op
    uint8_t texgen_fixed;  // F3DEX2 2.08: G_TEXTURE_GEN_LINEAR keeps S and T (2.04's BUG_TEXGEN_LINEAR_CLOBBER_S_T)
    uint8_t point_lights;  // F3DEX2 2.08PL: G_LIGHTING_POSITIONAL lights one light at a time, point lights among them
    uint8_t no_near;       // NoN: CLIP_NEAR marks w <= 0 and nothing clips at the near plane; cull_mask says if CULLDL tests it
};
extern const struct gfx_ucode *gfx_uc;

#define CLIP_NEAR 0x40u  // gvtx.clip's z <= -w; under NoN (gfx_uc->no_near) w <= 0 instead

// 128 bytes on 32-byte lines: the triangle tests read only the first line, a cached pv_of only the third
struct __attribute__((aligned(32))) gvtx {
    float x, y, z, w;  // clip space
    float nx, ny;      // x/w, y/w for the cull test
    uint16_t clip;
    uint8_t done, xy;  // done once color, s/t and screen exist; xy once iw, nx and ny do
    uint8_t ortho;     // loaded under an affine MP: w is constant, so depth comes from sz
    uint8_t batch;     // the G_VTX state it was loaded under
    float sx, sy, sz;  // screen: pixels, and the viewport's z / 4 (0..256 under a G_MAXZ viewport)
    float iw;          // 1/w
    int32_t s, t;      // texture coordinates, s10.5, after the G_TEXTURE scale
    uint8_t r, g, b, a;
    uint32_t pv_stamp __attribute__((aligned(32)));  // pv is this vertex's PVR form under draw state pv_stamp (gfx_draw.c); 0 = none
    struct gfx_pv pv;
    uint32_t raw_st, raw_c;  // the Vtx words for s/t and color/normal, finished lazily (gfx_vtx_finish)
};
_Static_assert(sizeof(struct gvtx) == 128, "gvtx: four cache lines");

struct rdp_tile {
    uint8_t fmt, siz, pal, cmt, cms, maskt, masks, shiftt, shifts;
    uint16_t line, tmem;   // in 64-bit words
    uint16_t uls, ult, lrs, lrt;  // 10.2
};

struct gfx_state {
    uint8_t dmem[0x1000] __attribute__((aligned(4)));  // the ucode's DMEM data, big-endian bytes
    float mv[4][4], proj[4][4], mp[4][4];
    float mv_stack[MV_STACK_CAP][4][4];
    int mv_depth, mv_max;
    uint32_t geom, omh, oml;
    uint8_t noclip;  // F3DLX with G_CLIPPING clear
    struct gvtx v[GFX_MAX_VTX];
    float vp_scale[3], vp_trans[3];  // the last G_VTX's viewport
    float dm_vs[3], dm_vt[3];        // DMEM's viewport now, and its fog words (gfx_vp_from_dmem)
    int16_t dm_fog[3];

    uint32_t cimg, cimg_w, cimg_siz, zimg;
    uint32_t timg, timg_fmt, timg_siz, timg_w;
    struct rdp_tile tile[8];
    uint8_t tmem[4096] __attribute__((aligned(8)));  // word-swapped like RDRAM: TMEM byte a is tmem[a ^ 3]
    uint32_t tmem_gen;  // bumps on every load, for the texture cache
    uint32_t cc_hi, cc_lo, fill, fog, blend, prim, env, prim_lod;
    uint16_t prim_z, prim_dz;
    uint16_t scis_ulx, scis_uly, scis_lrx, scis_lry;
    uint32_t rdp_omh, rdp_oml;
    uint32_t keygb, keyr, convert_lo;
    uint32_t rect_w0, rect_w1;
    int rect_pending;
};

extern struct gfx_state g;

extern uint32_t t_rdram_bias;
// Word-swapped RDRAM by physical address; nothing writes RDRAM while a task runs
static inline uint32_t gfx_rd32(uint32_t phys) { return *(const uint32_t *)(0x80000000u + phys + t_rdram_bias); }
static inline uint16_t gfx_rd16(uint32_t phys) { return *(const uint16_t *)(((0x80000000u + phys) ^ 2) + t_rdram_bias); }
static inline uint8_t gfx_rd8(uint32_t phys) { return *(const uint8_t *)(((0x80000000u + phys) ^ 3) + t_rdram_bias); }
static inline void gfx_prefetch(uint32_t phys) { __builtin_prefetch((const void *)(0x80000000u + phys + t_rdram_bias)); }

static inline uint8_t tm8(uint32_t a) { return g.tmem[(a & 0xFFF) ^ 3]; }
static inline uint16_t tm16(uint32_t a) { return *(const uint16_t *)(g.tmem + ((a & 0xFFE) ^ 2)); }
static inline void tm_set8(uint32_t a, uint8_t v) { g.tmem[(a & 0xFFF) ^ 3] = v; }

// What each TMEM load left where: the texture cache keys on these instead of hashing TMEM (gfx_rdp.c)
#define TMEM_RECS 16
struct tmem_rec { uint16_t lo, hi; uint32_t hash; uint8_t pend; };  // 64-bit word range [lo, hi) of TMEM
extern struct tmem_rec gfx_tmem_recs[TMEM_RECS];
extern int gfx_tmem_nrecs;
void gfx_tmem_materialize(void);  // loads only hash their source; this copies them into g.tmem before anything reads it
extern uint32_t gfx_load_hashed;  // loads whose source was hashed, not found in the memo

static inline uint16_t dmem16(uint32_t a) {
    if (!(a & 1)) return __builtin_bswap16(*(const uint16_t *)(g.dmem + (a & 0xFFE)));
    return (uint16_t)(g.dmem[a & 0xFFF] << 8 | g.dmem[(a + 1) & 0xFFF]);
}
static inline uint32_t dmem32(uint32_t a) {
    if (!(a & 3)) return __builtin_bswap32(*(const uint32_t *)(g.dmem + (a & 0xFFC)));
    return (uint32_t)dmem16(a) << 16 | dmem16(a + 2);
}
// len bytes of RDRAM from src into DMEM at dst, wrapping at 4 KB: a byte-swapped word at a time when all are aligned
static inline void gfx_dmem_load(uint32_t dst, uint32_t src, uint32_t len) {
    if (!((dst | src | len) & 3))
        for (uint32_t i = 0; i < len; i += 4)
            *(uint32_t *)(g.dmem + ((dst + i) & 0xFFC)) = __builtin_bswap32(gfx_rd32(src + i));
    else
        for (uint32_t i = 0; i < len; i++) g.dmem[(dst + i) & 0xFFF] = gfx_rd8(src + i);
}
static inline void dmem_set32(uint32_t a, uint32_t v) {
    for (int i = 0; i < 4; i++) g.dmem[(a + i) & 0xFFF] = (uint8_t)(v >> (24 - 8 * i));
}
void os_die(const char *why, uint32_t v) __attribute__((noreturn));
static inline void gfx_need(uint32_t a, uint32_t n, const char *why) {
    if (a >= RDRAM_SIZE || n > RDRAM_SIZE - a) os_die(why, a);
}
// segmented_to_physical through the ucode's segment table at DMEM seg; the DMA engine then sees 24 bits
static inline uint32_t gfx_seg_phys(uint32_t seg, uint32_t a) {
    return ((a & 0x00FFFFFF) + dmem32(seg + ((a >> 22) & 0x3C))) & 0x00FFFFFF;
}

void gfx_load_mtx(uint32_t phys, uint32_t flags);
void gfx_pop_mtx(void);
void gfx_mp_from_dmem(void);
void gfx_update_mp(void);
// F3DEX2: G_MTX leaves MP to the next G_VTX; matrices also live in DMEM, as s15.16 Mtx (gfx_mtx_to_dmem fills them in)
void gfx_load_mtx2(uint32_t phys, int proj, int load, int push);
int gfx_pop_mtx2(uint32_t n);  // 0: the pointer didn't move
void gfx_mtx_from_dmem(uint32_t a, float m[4][4]);
void gfx_mtx_to_dmem(uint32_t a, const float m[4][4]);
void gfx_vp_from_dmem(void);
// tex: G_TEXTURE's s/t scale word; both from the dispatcher, which reads them at its ucode's constant addresses
void gfx_load_vtx(uint32_t phys, uint32_t n, uint32_t v0, uint32_t tex, int16_t clip_ratio);
extern void (*gfx_vtx_finish)(struct gvtx *v);  // set per task by gfx_vtx_reset
void gfx_modify_vtx(uint32_t off, uint32_t val);  // G_MW_POINTS: one word of a loaded vertex, nothing recomputed
static inline __attribute__((always_inline)) void gfx_vtx_need(struct gvtx *v) { if (!v->done) gfx_vtx_finish(v); }
// The part of the finish the cull needs, so a culled triangle's vertices skip the rest
static inline __attribute__((always_inline)) void gfx_vtx_xy_now(struct gvtx *v) {
    if (v->xy) return;
    v->xy = 1;
    float iw = v->w != 0.0f ? 1.0f / v->w : 0.0f;
    v->iw = iw, v->nx = v->x * iw, v->ny = v->y * iw;
}
void gfx_vtx_xy_call(struct gvtx *v);  // out of line, to keep gfx_tri small
static inline __attribute__((always_inline)) void gfx_vtx_xy(struct gvtx *v) { if (!v->xy) gfx_vtx_xy_call(v); }
void gfx_vtx_reset(void);  // at task start: the RSP's points buffer doesn't survive a task
// Where the vertex batches sit
extern const void *const gfx_batches;
extern const uint32_t gfx_batches_bytes;
#define NO_BATCH 0xFF
int gfx_cull_dl(uint32_t vfirst, uint32_t vlast);
void gfx_tri(int a, int b, int c, int provoking);
void gfx_tri_noclip(int a, int b, int c, int provoking);  // F3DLX with G_CLIPPING clear: no trivial reject, no clip
float gfx_tri_cross(int a, int b, int c);  // the cull test's signed area

void gfx_rdp(uint32_t w0, uint32_t w1);
void gfx_texrect(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3);
void gfx_fillrect(uint32_t w0, uint32_t w1);
void gfx_scissor(void);  // after G_SETSCISSOR or a new frame
void gfx_guard_band(float ratio);  // per G_VTX, under its viewport

void gfx_frame_begin(void);
void gfx_frame_end(void);
extern int gfx_dirty;  // the draw state needs resolving before the next draw
static inline void gfx_state_changed(void) { gfx_dirty = 1; }
extern uint32_t gfx_drawn_tris, gfx_rej_cull;

#endif
