// Triangles and rectangles: reject, cull, near/far clip, resolve the PVR state, emit to the sink.
#include "gfx.h"
#include "gfx_cc.h"
#include "gfx_tex.h"
#include "gfx_sink.h"
#include "f3d.h"
#include "os.h"

#define OML_Z_SRC_PRIM 0x04u
#define OML_Z_CMP 0x10u
#define OML_Z_UPD 0x20u
#define CYC_COPY 2u
#define CYC_FILL 3u
#define DECAL_BIAS 1.001f  // pulls ZMODE_DEC surfaces in front of the coplanar surface under them
#define MAX_CLIP 9
#define CLIP_XY 0x0303u   // x, y beyond the clip ratio (gfx_xform.c clip_code)
#define CLIP_ANY 0x4343u  // those, and z beyond near or far
#define BG_Z0 1e-6f            // background depth: above the 1e-7 clear plane, below any 1/w nearer than w = 200000
#define BG_STEP (BG_Z0 / 1024.0f)
#define BG_LAYERS 4096
#define Z_EPOCH_SCALE 67108864.0f  // 2^26: an epoch's depths, 1e-7 to ~1, all lie under the next one's
#define Z_EPOCHS 4                 // 2^104 times ~1 stays inside a float

const struct gfx_sink *gfx_sink;
uint32_t gfx_drawn_tris, gfx_rej_cull;

int gfx_dirty = 1;
static int framed, tr_seen, z_seen;  // a TR draw, and a depth-tested draw, so far this depth epoch
static struct cc_plan plan;
static struct tex_bind tb;
static struct gfx_poly cur, last;
static int have_last;
static int poly_fresh;  // cur may differ from last: every write to cur, and every have_last reset, sets it
static int bg_layer;
static float bg_z;  // the depth every vertex of the current draw takes (background or primitive depth), 0 if none
static int z_epochs;
static float z_scale = 1.0f;  // the current depth epoch's: a uniform scale leaves the PVR's perspective unchanged
static float z_mul = 1.0f;    // z_scale, with the decal bias of the resolved draw


// A Z clear over depth-tested draws: later draws paint over them, so they restart the painter's background and take
// depths above all of theirs
static void z_epoch(void) {
    tr_seen = z_seen = 0;
    if (z_epochs < Z_EPOCHS) z_epochs++, z_scale *= Z_EPOCH_SCALE;
    gfx_dirty = 1;
}

// Unsorted draws before any depth-tested one are painter's background, a layer each: OP ones leave the top layer in
// depth, which PT and TR test against, so MK64's later OP ground covers its earlier TR clouds though TR draws last
static int background(void) {
    bg_z = 0.0f;
    if (z_seen || cur.depth_cmp != DC_ALWAYS || cur.depth_write) return 0;
    bg_layer += bg_layer < BG_LAYERS;
    bg_z = (BG_Z0 + (float)bg_layer * BG_STEP) * z_scale;
    cur.depth_write = cur.list != GL_TR;
    if (cur.list != GL_OP) cur.depth_cmp = DC_GEQUAL;
    return 1;
}

struct cv { float x, y, z, w, s, t; float c[4]; uint8_t ortho, screen; };

static int use_fill;
static uint32_t fill_argb;
static uint32_t draw_stamp = 1;  // advances with every resolved draw state; cached vertex forms carry the one they used

static void next_stamp(void) {
    if (++draw_stamp == 0) draw_stamp = 1;
}

struct gvtx;
static void to_cv(const struct gvtx *v, const struct gvtx *flat, struct cv *o);
GFX_COLD static void tri_slow(const struct gvtx *a, const struct gvtx *b, const struct gvtx *c, const struct gvtx *flat);
static float tex_iw, tex_ih;  // 1 / the bound texture's size: powers of two, so the products equal the divisions
static float rect_off_s, rect_off_t;  // the current rect's sampling offsets, in texels
static float rect_phase_s, rect_phase_t, rect_inv_step_s = 1.0f, rect_inv_step_t = 1.0f;  // its decimation, as bound
static const struct tex_dec no_dec = { 1, 1, 0, 0 };
static struct tex_dec rect_dec = { 1, 1, 0, 0 };  // what the rect being resolved asks the binder for
#define RECT_Z (1.0f / 1024.0f)

static float scis_x0, scis_y0, scis_x1, scis_y1;
static int scis_on;  // some scissor edge lies inside the frame; the PVR's own frame clip covers the rest
static struct gfx_cover drawn;     // cells the frame's triangles touched, by their bounding boxes
static int cover_full;             // every cell of the scissor is drawn: triangles stop marking
static float cover_kx, cover_ky;   // cells per pixel
static uint32_t scis_m;            // the scissor's cells: these columns of rows scis_r0..scis_r1
static int scis_r0, scis_r1;

// The columns, and rows *r0..*r1, a box in pixels touches; the last cell is the one before an exact edge
static uint32_t cover_box(float x0, float y0, float x1, float y1, int *r0, int *r1) {
    float fc0 = x0 * cover_kx, fc1 = x1 * cover_kx - 1.0f / 64, fr0 = y0 * cover_ky, fr1 = y1 * cover_ky - 1.0f / 64;
    if (fc1 < 0.0f || fr1 < 0.0f || fc0 >= GFX_COVER_COLS || fr0 >= GFX_COVER_ROWS) return 0;
    int c0 = fc0 > 0.0f ? (int)fc0 : 0, c1 = fc1 < GFX_COVER_COLS - 1 ? (int)fc1 : GFX_COVER_COLS - 1;
    *r0 = fr0 > 0.0f ? (int)fr0 : 0, *r1 = fr1 < GFX_COVER_ROWS - 1 ? (int)fr1 : GFX_COVER_ROWS - 1;
    if (c1 < c0) c1 = c0;
    if (*r1 < *r0) *r1 = *r0;
    return ((2u << c1) - 1) & ~((1u << c0) - 1);
}

static int scissor_drawn(void) {
    for (int r = scis_r0; r <= scis_r1; r++)
        if (scis_m & ~drawn.row[r]) return 0;
    return 1;
}

// Marks the cells a drawn triangle's bounding box touches
GFX_COLD static void cover_tri(const struct gfx_pv *a, const struct gfx_pv *b, const struct gfx_pv *c) {
    float x0 = a->x < b->x ? a->x : b->x, x1 = a->x < b->x ? b->x : a->x;
    float y0 = a->y < b->y ? a->y : b->y, y1 = a->y < b->y ? b->y : a->y;
    x0 = c->x < x0 ? c->x : x0, x1 = c->x > x1 ? c->x : x1, y0 = c->y < y0 ? c->y : y0, y1 = c->y > y1 ? c->y : y1;
    int r0 = 0, r1 = -1;
    uint32_t m = cover_box(x0, y0, x1, y1, &r0, &r1);
    for (int r = r0; r <= r1; r++) drawn.row[r] |= m;
    if ((m & scis_m) == scis_m) cover_full = scissor_drawn();
}

static int frame_rows;  // the frame's lowest scissor edge: the RDP writes no row of its color image below it

void gfx_scissor(void) {
    float w = (float)g.cimg_w, h = (float)(g.cimg_w * 3 / 4);
    float x0 = (float)g.scis_ulx * 0.25f, y0 = (float)g.scis_uly * 0.25f;
    float x1 = (float)g.scis_lrx * 0.25f, y1 = (float)g.scis_lry * 0.25f;
    int rows = ((int)g.scis_lry + 3) >> 2;
    if (rows > frame_rows) frame_rows = rows;
    scis_on = x0 > 0.0f || y0 > 0.0f || x1 < w || y1 < h;
    scis_x0 = x0 > 0.0f ? x0 : -1e30f, scis_y0 = y0 > 0.0f ? y0 : -1e30f;
    scis_x1 = x1 < w ? x1 : 1e30f, scis_y1 = y1 < h ? y1 : 1e30f;
    scis_m = cover_box(x0, y0, x1, y1, &scis_r0, &scis_r1);
    cover_full = !scis_m || scissor_drawn();
}

static uint16_t clip_slow = CLIP_ANY;  // clip codes that send a triangle through clip space

// A guard band that encloses the frame clips nothing visible: the PVR's frame clip is enough
void gfx_guard_band(float ratio) {
    float gx = ratio * g.vp_scale[0], gy = ratio * g.vp_scale[1];
    gx = gx < 0.0f ? -gx : gx, gy = gy < 0.0f ? -gy : gy;
    float w = (float)g.cimg_w, h = (float)(g.cimg_w * 3 / 4);
    int off = g.vp_trans[0] - gx <= 0.0f && g.vp_trans[0] + gx >= w && g.vp_trans[1] - gy <= 0.0f && g.vp_trans[1] + gy >= h;
    clip_slow = off ? CLIP_ANY & ~CLIP_XY : CLIP_ANY;
}

static void ensure_frame(void) {
    if (framed) return;
    framed = 1;
    int tiny = g.cimg_w < 4;  // no grid: counts as all drawn
    __builtin_memset(&drawn, tiny ? 0xFF : 0, sizeof drawn);
    cover_kx = tiny ? 0.0f : (float)GFX_COVER_COLS / (float)g.cimg_w;
    cover_ky = tiny ? 0.0f : (float)GFX_COVER_ROWS / (float)(g.cimg_w * 3 / 4);
    frame_rows = 0;
    gfx_scissor();
    gfx_sink->frame_begin(g.cimg, (int)g.cimg_siz, (int)g.cimg_w, (int)(g.cimg_w * 3 / 4));  // 4:3; the scissor can change mid-frame
}

void gfx_frame_begin(void) {
    gfx_tex_frame();
    framed = 0;
    tr_seen = z_seen = 0;
    bg_layer = 0, bg_z = 0.0f;
    z_epochs = 0, z_scale = 1.0f;
    next_stamp();
    have_last = 0, poly_fresh = 1;
    gfx_dirty = 1;
}

void gfx_frame_end(void) {
    if (!framed) return;
    gfx_sink->frame_end(&drawn, frame_rows);
}

// SETPRIMDEPTH's z as a vertex's depth: RDP z is sz * 128, and a perspective MP gives z/w = a + b/w
static float prim_depth(void) {
    float sz = (float)(g.prim_z & 0x7FFF) * (1.0f / 128.0f);
    int k = 0;
    for (int i = 1; i < 3; i++)
        if (__builtin_fabsf(g.mp[i][3]) > __builtin_fabsf(g.mp[k][3])) k = i;
    if (g.mp[k][3] == 0.0f) return (1024.0f - sz) * (1.0f / 1024.0f);
    float a = g.mp[k][2] / g.mp[k][3], b = g.mp[3][2] - a * g.mp[3][3];
    float iw = b != 0.0f ? ((sz - g.vp_trans[2]) / g.vp_scale[2] - a) / b : 0.0f;
    return iw > 0.0f ? iw : BG_STEP;  // behind the eye: under every surface, as no 1/w reaches it
}

static void resolve(int rect, int tex_on) {
    uint32_t cyc = (g.rdp_omh >> 20) & 3;
    gfx_cc_plan(&plan, tex_on, rect);
    __builtin_memset(&cur, 0, sizeof cur);
    poly_fresh = 1;  // also covers what texrect changes in cur after this
    cur.list = plan.list, cur.src = plan.src, cur.dst = plan.dst;
    cur.txr_env = plan.env, cur.offset = plan.offset, cur.fog = plan.fog;
    cur.ignore_tex_alpha = plan.ignore_tex_alpha;
    if (rect && tex_on && cyc == CYC_COPY) {
        plan.textured = 1;
        cur.txr_env = TXR_REPLACE, cur.offset = 0, cur.fog = 0;
        cur.list = (g.rdp_oml & 3) == 1 ? GL_PT : GL_OP;
        cur.src = BL_ONE, cur.dst = BL_ZERO;
    }
    if (plan.textured) {
        uint32_t tile = rect ? (g.rect_w1 >> 24) & 7 : (dmem32(gfx_uc->texture) >> 8) & 7;
        int rgb_off = 0, alpha_off = 0;
        if (cyc != CYC_COPY) gfx_cc_texel_tiles(&rgb_off, &alpha_off);
        int alpha_tile = alpha_off != rgb_off ? (int)((tile + (uint32_t)alpha_off) & 7) : -1;
        tile = (tile + (uint32_t)rgb_off) & 7;
        struct tex_fx fx = { plan.alpha_test ? (int16_t)(g.blend & 0xFF) : (int16_t)-1, plan.tex_invert };
        gfx_tex_bind((int)tile, alpha_tile, fx, rect ? &rect_dec : &no_dec, &tb);
        cur.textured = 1;
        cur.tex_id = tb.id, cur.tex_w = tb.w, cur.tex_h = tb.h;
        tex_iw = 1.0f / (float)tb.w, tex_ih = 1.0f / (float)tb.h;
        cur.filter = rect && cyc == CYC_COPY ? 0 : tb.filter;
        cur.clamp_u = tb.clamp_u, cur.clamp_v = tb.clamp_v, cur.flip_u = tb.flip_u, cur.flip_v = tb.flip_v;
    }
    int zprim = (g.rdp_oml & OML_Z_SRC_PRIM) != 0;  // the RDP takes SETPRIMDEPTH's z, for rects as for triangles
    int zbuf = zprim || (!rect && (g.geom & G_ZBUFFER) != 0);
    cur.depth_cmp = zbuf && (g.rdp_oml & OML_Z_CMP) ? DC_GEQUAL : DC_ALWAYS;
    cur.depth_write = zbuf && (g.rdp_oml & OML_Z_UPD);
    int amid = !rect && !zbuf && z_seen;  // a surface outside the z pipeline amid depth-tested drawing
    int tr_write = plan.list == GL_TR && cur.depth_write;
    // Before any depth-tested draw, unsorted 2D is background (MK64's sky and ground) that the scene must cover. PT
    // always tests >=, so later unsorted PT would lose to the depths before it (Rayman's eye rects)
    if (cur.depth_cmp == DC_ALWAYS && z_seen && (tr_seen || cur.list == GL_PT) && cur.list != GL_TR) {
        int cutout = cur.list == GL_PT;  // TR has no alpha test: a cutout keeps its holes by blending on texel alpha
        cur.list = GL_TR;  // the PVR draws TR after OP: keep later unsorted 2D on top
        cur.src = cutout ? BL_SRCALPHA : BL_ONE, cur.dst = cutout ? BL_INVSRCALPHA : BL_ZERO;
        if (cutout) cur.ignore_tex_alpha = 0;
    }
    // A TR surface that writes depth or is 3D outside the z pipeline draws after OP and PT: a depth test keeps what the
    // N64 drew over it later on top, and the decal bias keeps it over the coplanar ground it was painted on
    int tr_underlay = cur.list == GL_TR && cur.depth_cmp == DC_ALWAYS && (tr_write || amid);
    if (cur.list == GL_PT) cur.src = BL_SRCALPHA, cur.dst = BL_INVSRCALPHA;  // the PVR skips the PT alpha test under ONE/ZERO
    if (cur.list == GL_TR && plan.alpha_cvg) cur.depth_write = 0;  // TR has no alpha test: it would write the holes' depth
    if (cur.list == GL_TR) tr_seen = 1;
    if (cur.depth_cmp != DC_ALWAYS) z_seen = 1;
    int layered = background();  // its depth is the layer's: G_ZS_PRIM left on by a z-less draw doesn't move it
    if (tr_underlay) cur.depth_cmp = DC_GEQUAL, cur.underlay = 1;
    z_mul = plan.zmode == 3 || (tr_underlay && amid) ? z_scale * DECAL_BIAS : z_scale;
    if (zprim && zbuf && !layered && (cur.depth_cmp != DC_ALWAYS || cur.depth_write)) bg_z = prim_depth() * z_mul;
    cur.fog_argb = g.fog;
    gfx_dirty = 0;
    next_stamp();
}

GFX_HOT(06) static void emit_poly(void) {
    if (!poly_fresh) return;
    poly_fresh = 0;
    if (have_last && __builtin_memcmp(&cur, &last, sizeof cur) == 0) return;
    last = cur;
    have_last = 1;
    gfx_sink->poly(&cur);
}

static float shift_st(float v, uint32_t shift) {
    if (shift == 0) return v;
    return shift <= 10 ? v / (float)(1u << shift) : v * (float)(1u << (16 - shift));
}

static void to_pv(const struct cv *c, struct gfx_pv *o) {
    if (c->screen) {
        o->x = c->x, o->y = c->y, o->z = RECT_Z;
    } else {
        float iw = c->w != 0.0f ? 1.0f / c->w : 0.0f;
        o->x = c->x * iw * g.vp_scale[0] + g.vp_trans[0];
        o->y = c->y * iw * g.vp_scale[1] + g.vp_trans[1];
        float sz = c->z * iw * g.vp_scale[2] + g.vp_trans[2];
        float z = c->ortho ? (1024.0f - sz) * (1.0f / 1024.0f) : iw;
        o->z = z * z_mul;
    }
    if (bg_z != 0.0f) o->z = bg_z;
    if (use_fill) {
        o->u = o->v = 0.0f, o->argb = fill_argb, o->oargb = 0;
        return;
    }
    if (cur.textured) {
        float off_s = cur.filter ? 0.5f : 0.0f, off_t = off_s;
        float rs = (shift_st((float)(int32_t)c->s, tb.shifts) - (float)(tb.uls << 3)) * (1.0f / 32.0f);
        float rt = (shift_st((float)(int32_t)c->t, tb.shiftt) - (float)(tb.ult << 3)) * (1.0f / 32.0f);
        if (c->screen) {
            off_s = rect_off_s, off_t = rect_off_t;
            rs = (rs - rect_phase_s) * rect_inv_step_s, rt = (rt - rect_phase_t) * rect_inv_step_t;
        }
        o->u = (rs + off_s) * tex_iw;
        o->v = (rt + off_t) * tex_ih;
    } else {
        o->u = o->v = 0.0f;
    }
    uint8_t shade[4];
    for (int k = 0; k < 4; k++) {
        float v = c->c[k];
        shade[k] = (uint8_t)(v < 0.0f ? 0 : v > 255.0f ? 255 : (int)(v + 0.5f));
    }
    gfx_cc_vertex(&plan, shade, &o->argb, &o->oargb);
}

static void lerp_cv(struct cv *o, const struct cv *a, const struct cv *b, float t) {
    o->x = a->x + (b->x - a->x) * t, o->y = a->y + (b->y - a->y) * t;
    o->z = a->z + (b->z - a->z) * t, o->w = a->w + (b->w - a->w) * t;
    o->s = a->s + (b->s - a->s) * t, o->t = a->t + (b->t - a->t) * t;
    for (int k = 0; k < 4; k++) o->c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
    o->ortho = a->ortho, o->screen = 0;
}

static float cv_axis(const struct cv *v, int axis) { return axis == 0 ? v->x : axis == 1 ? v->y : v->z; }

// Sutherland-Hodgman against c >= -r * w (side -1) or c <= r * w (side 1), c the axis component
static int clip_plane(struct cv *out, const struct cv *in, int n, int axis, float side, float r) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const struct cv *a = &in[i], *b = &in[(i + 1) % n];
        float da = r * a->w - side * cv_axis(a, axis), db = r * b->w - side * cv_axis(b, axis);
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0) && m < MAX_CLIP) lerp_cv(&out[m++], a, b, da / (da - db));
    }
    return m;
}

static uint32_t lerp_argb(uint32_t a, uint32_t b, float p) {
    uint32_t o = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
        o |= (uint32_t)(int)(ca + (cb - ca) * p + 0.5f) << sh;
    }
    return o;
}

// t is screen-linear; the PVR interpolates u, v and color perspective-correctly by z, so those take p
static void lerp_pv(struct gfx_pv *o, const struct gfx_pv *a, const struct gfx_pv *b, float t) {
    float z = a->z + (b->z - a->z) * t, p = z != 0.0f ? t * b->z / z : t;
    o->x = a->x + (b->x - a->x) * t, o->y = a->y + (b->y - a->y) * t, o->z = z;
    o->u = a->u + (b->u - a->u) * p, o->v = a->v + (b->v - a->v) * p;
    o->argb = lerp_argb(a->argb, b->argb, p), o->oargb = lerp_argb(a->oargb, b->oargb, p);
}

// Keeps the side where sign * (coordinate - lim) >= 0
static int clip_scis(struct gfx_pv *out, const struct gfx_pv *in, int n, int axis_y, float lim, float sign) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const struct gfx_pv *a = &in[i], *b = &in[i + 1 < n ? i + 1 : 0];
        float da = sign * ((axis_y ? a->y : a->x) - lim), db = sign * ((axis_y ? b->y : b->x) - lim);
        if (da >= 0.0f) out[m++] = *a;
        if ((da >= 0.0f) != (db >= 0.0f)) lerp_pv(&out[m++], a, b, da / (da - db));
    }
    return m;
}

// The RSP clips only to its guard band and leaves the rest to the RDP scissor
GFX_COLD static void emit_scissored(const struct gfx_pv *a, const struct gfx_pv *b, const struct gfx_pv *c) {
    struct gfx_pv p[8], q[8], v[3] = { *a, *b, *c };
    int n = clip_scis(q, v, 3, 0, scis_x0, 1.0f);
    n = clip_scis(p, q, n, 0, scis_x1, -1.0f);
    n = clip_scis(q, p, n, 1, scis_y0, 1.0f);
    n = clip_scis(p, q, n, 1, scis_y1, -1.0f);
    if (n < 3) return;
    for (int i = 1; i + 1 < n; i++) {
        struct gfx_pv t[3] = { p[0], p[i], p[i + 1] };
        if ((t[1].x - t[0].x) * (t[2].y - t[0].y) == (t[1].y - t[0].y) * (t[2].x - t[0].x)) continue;
        emit_poly();
        gfx_sink->tri(&t[0], &t[1], &t[2]);
        gfx_drawn_tris++;
        if (!cover_full) cover_tri(&t[0], &t[1], &t[2]);
    }
}

static inline int outside_scissor(const struct gfx_pv *v) {
    return v->x < scis_x0 || v->x > scis_x1 || v->y < scis_y0 || v->y > scis_y1;
}

GFX_HOT(07) __attribute__((noinline)) static void emit3(const struct gfx_pv *a, const struct gfx_pv *b, const struct gfx_pv *c) {
    float cross = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (cross == 0.0f) return;  // fast3d.s skips zero-area triangles
    if (scis_on && (outside_scissor(a) || outside_scissor(b) || outside_scissor(c))) {
        emit_scissored(a, b, c);
        return;
    }
    if (poly_fresh) emit_poly();
    gfx_sink->tri(a, b, c);
    gfx_drawn_tris++;
    if (__builtin_expect(!cover_full, 0)) cover_tri(a, b, c);
}

static void emit_pv(const struct gfx_pv *v) { emit3(&v[0], &v[1], &v[2]); }

// A loaded vertex's PVR form under the current draw state: the same arithmetic as to_cv + to_pv, done once per vertex
GFX_HOT(04) __attribute__((noinline)) static void pv_build(struct gvtx *v) {
    struct gfx_pv *o = &v->pv;
    o->x = v->sx, o->y = v->sy;  // screen position as the vertex load computed it, as fast3d.s keeps it
    float z = v->ortho ? (1024.0f - v->sz) * (1.0f / 1024.0f) : v->iw;
    o->z = bg_z != 0.0f ? bg_z : z * z_mul;
    if (cur.textured) {
        float off = cur.filter ? 0.5f : 0.0f;
        o->u = ((shift_st(v->s, tb.shifts) - (float)(tb.uls << 3)) * (1.0f / 32.0f) + off) * tex_iw;
        o->v = ((shift_st(v->t, tb.shiftt) - (float)(tb.ult << 3)) * (1.0f / 32.0f) + off) * tex_ih;
    } else {
        o->u = o->v = 0.0f;
    }
    uint8_t shade[4] = { 0, 0, 0, 0 };
    if (g.geom & G_SHADE) shade[0] = v->r, shade[1] = v->g, shade[2] = v->b, shade[3] = v->a;
    gfx_cc_vertex(&plan, shade, &o->argb, &o->oargb);
    v->pv_stamp = draw_stamp;
}

static void emit_tri(const struct cv *a, const struct cv *b, const struct cv *c) {
    struct gfx_pv v[3];
    to_pv(a, &v[0]);
    to_pv(b, &v[1]);
    to_pv(c, &v[2]);
    emit_pv(v);
}

static void to_cv(const struct gvtx *v, const struct gvtx *flat, struct cv *o) {
    const struct gvtx *col = flat ? flat : v;
    o->x = v->x, o->y = v->y, o->z = v->z, o->w = v->w;
    o->s = (float)v->s, o->t = (float)v->t;
    if (g.geom & G_SHADE) {
        o->c[0] = col->r, o->c[1] = col->g, o->c[2] = col->b;
    } else {
        o->c[0] = o->c[1] = o->c[2] = 0;
    }
    o->c[3] = (g.geom & G_SHADE) ? (gfx_uc->flat_rgb ? v : col)->a : 0;
    o->ortho = v->ortho, o->screen = 0;
}

static inline __attribute__((always_inline)) float cull_cross(const struct gvtx *a, const struct gvtx *b, const struct gvtx *c) {
    float ax = a->nx, ay = a->ny, bx = b->nx, by = b->ny, cx = c->nx, cy = c->ny;
    float cross = (ax - bx) * (cy - by) - (ay - by) * (cx - bx);  // relative to the second vertex, as sm64-port's gfx_pc.c
    return ((a->w < 0) ^ (b->w < 0) ^ (c->w < 0)) ? -cross : cross;
}

GFX_COLD float gfx_tri_cross(int ia, int ib, int ic) { return cull_cross(&g.v[ia], &g.v[ib], &g.v[ic]); }

GFX_HOT(03) void gfx_tri(int ia, int ib, int ic, int provoking) {
    const struct gvtx *a = &g.v[ia], *b = &g.v[ib], *c = &g.v[ic];
    if (a->clip & b->clip & c->clip & 0x7070) return;  // all outside the same plane
    uint32_t cull = g.geom & (G_CULL_FRONT | G_CULL_BACK);
    if (cull) {
        gfx_vtx_xy(&g.v[ia]);
        gfx_vtx_xy(&g.v[ib]);
        gfx_vtx_xy(&g.v[ic]);
        float cross = cull_cross(a, b, c);
        if (cull == (G_CULL_FRONT | G_CULL_BACK) || (cull == G_CULL_FRONT && cross <= 0) || (cull == G_CULL_BACK && cross >= 0)) {
            gfx_rej_cull++;
            return;
        }
    }
    gfx_vtx_need(&g.v[ia]);
    gfx_vtx_need(&g.v[ib]);
    gfx_vtx_need(&g.v[ic]);
    ensure_frame();
    if (gfx_dirty) resolve(0, (g.geom & G_TEXTURE_ENABLE) != 0);
    const struct gvtx *flat = (g.geom & (G_SHADING_SMOOTH | G_SHADE)) == G_SHADE ? &g.v[provoking] : 0;  // unshaded: no color to flatten
    if (!flat && !((a->clip | b->clip | c->clip) & clip_slow)) {  // the common case: shared vertices, converted once
        struct gvtx *va = &g.v[ia], *vb = &g.v[ib], *vc = &g.v[ic];
        if (va->pv_stamp != draw_stamp) pv_build(va);
        if (vb->pv_stamp != draw_stamp) pv_build(vb);
        if (vc->pv_stamp != draw_stamp) pv_build(vc);
        emit3(&va->pv, &vb->pv, &vc->pv);
        return;
    }
    tri_slow(a, b, c, flat);
}

// F3DLX with G_CLIPPING clear: its triangle routine reads no clip codes, so neither rejects nor clips
GFX_COLD void gfx_tri_noclip(int ia, int ib, int ic, int provoking) {
    uint16_t ca = g.v[ia].clip, cb = g.v[ib].clip, cc = g.v[ic].clip;
    g.v[ia].clip = g.v[ib].clip = g.v[ic].clip = 0;
    gfx_tri(ia, ib, ic, provoking);
    g.v[ia].clip = ca, g.v[ib].clip = cb, g.v[ic].clip = cc;
}

// Flat shading, or a triangle crossing the guard band, near or far: through clip space
static void tri_slow(const struct gvtx *a, const struct gvtx *b, const struct gvtx *c, const struct gvtx *flat) {
    static const struct { uint16_t bit; uint8_t axis; int8_t side; } planes[6] = {
        { 0x01, 0, -1 }, { 0x100, 0, 1 }, { 0x02, 1, -1 }, { 0x200, 1, 1 }, { 0x40, 2, -1 }, { 0x4000, 2, 1 },
    };
    struct cv buf[2][MAX_CLIP], *p = buf[0], *q = buf[1];
    to_cv(a, flat, &p[0]);
    to_cv(b, flat, &p[1]);
    to_cv(c, flat, &p[2]);
    int n = 3;
    uint16_t code = (a->clip | b->clip | c->clip) & clip_slow;
    if ((code & CLIP_NEAR) && gfx_uc->no_near)  // NoN: x, y at the guard band cut what's behind the eye
        code = (uint16_t)((code & ~CLIP_NEAR) | ((a->clip | b->clip | c->clip) & CLIP_XY));
    float ratio = code & CLIP_XY ? (float)(int16_t)dmem16(gfx_uc->clip_ratio) : 1.0f;  // x, y clip at the guard band
    for (int k = 0; k < 6 && n >= 3; k++) {
        if (!(code & planes[k].bit)) continue;
        n = clip_plane(q, p, n, planes[k].axis, (float)planes[k].side, planes[k].axis < 2 ? ratio : 1.0f);
        struct cv *t = p;
        p = q, q = t;
    }
    for (int i = 1; i + 1 < n; i++) emit_tri(&p[0], &p[i], &p[i + 1]);
}

static void rect_vertex(struct cv *o, float x, float y, float s, float t) {
    o->x = x, o->y = y, o->z = 0.0f, o->w = 1.0f, o->s = s, o->t = t, o->ortho = 1, o->screen = 1;
    for (int k = 0; k < 4; k++) o->c[k] = 0;
}

static void rect_quad(float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1, int flip) {
    struct cv v[4];
    rect_vertex(&v[0], x0, y0, s0, t0);
    rect_vertex(&v[1], x1, y0, flip ? s0 : s1, flip ? t1 : t0);
    rect_vertex(&v[2], x0, y1, flip ? s1 : s0, flip ? t0 : t1);
    rect_vertex(&v[3], x1, y1, s1, t1);
    emit_tri(&v[0], &v[1], &v[2]);
    emit_tri(&v[2], &v[1], &v[3]);
}

void gfx_fillrect(uint32_t w0, uint32_t w1) {
    uint32_t cyc = (g.rdp_omh >> 20) & 3;
    float lrx = (float)((w0 >> 12) & 0xFFF) * 0.25f, lry = (float)(w0 & 0xFFF) * 0.25f;
    float ulx = (float)((w1 >> 12) & 0xFFF) * 0.25f, uly = (float)(w1 & 0xFFF) * 0.25f;
    if (cyc == CYC_FILL && g.cimg == g.zimg) {
        if (z_seen) z_epoch();
        return;
    }
    ensure_frame();
    if (cyc >= CYC_COPY) lrx += 1.0f, lry += 1.0f;
    if (cyc != CYC_FILL) resolve(1, 0);
    if (cyc == CYC_FILL) {
        uint32_t c = g.fill;
        if (g.cimg_siz == 3) {
            fill_argb = (c & 0xFF) << 24 | c >> 8;
        } else {
            uint32_t p = c >> 16, r = (p >> 11) & 31, gg = (p >> 6) & 31, b = (p >> 1) & 31;
            fill_argb = 0xFF000000u | (r << 3 | r >> 2) << 16 | (gg << 3 | gg >> 2) << 8 | (b << 3 | b >> 2);
        }
        __builtin_memset(&cur, 0, sizeof cur);
        poly_fresh = 1;
        cur.list = tr_seen && z_seen ? GL_TR : GL_OP, cur.src = BL_ONE, cur.dst = BL_ZERO;
        cur.txr_env = TXR_MODULATE, cur.depth_cmp = DC_ALWAYS;
        background();
        use_fill = 1;
        rect_quad(ulx, uly, lrx, lry, 0, 0, 0, 0, 0);
        use_fill = 0;
    } else {
        rect_quad(ulx, uly, lrx, lry, 0, 0, 0, 0, 0);
    }
    gfx_dirty = 1;
}

static int whole_texel(float st) { return st == (float)(int32_t)st && ((int32_t)st & 31) == 0; }
static int whole_step(float d) { return d != 0.0f && d == (float)(int32_t)d; }
// Whether n pixels sampling r0, r0 + d, ... (and each one's bilinear neighbor) stay inside a tile of size texels
static int32_t ceil_pos(float v) { return (int32_t)v + ((float)(int32_t)v < v); }  // v >= 0, no libm

static int in_tile(float r0, float d, int n, uint32_t size, int filter) {
    float last = r0 + (float)(n - 1) * d, lo = d < 0.0f ? last : r0, hi = d < 0.0f ? r0 : last;
    return n > 0 && lo >= 0.0f && (filter ? ceil_pos(hi) : (int32_t)hi) <= (int32_t)size - 1;
}

static uint8_t dec_step(float d) { return d >= -8.0f && d <= 8.0f ? (uint8_t)(d < 0.0f ? -d : d) : 1; }

static uint8_t dec_phase(float st0, float d) {
    int32_t step = dec_step(d), texel = (int32_t)st0 >> 5;
    return (uint8_t)(((texel % step) + step) % step);
}

// The RDP steps a rect's s, t from each pixel's corner and the PVR samples at its center: centered, half a step back;
// otherwise (point-sampled, fractional steps) the first texel kept under every sub-pixel of an upscaled pixel
static float rect_off(float d, int centered) { return centered ? 0.5f - 0.5f * d : d < 0.0f ? -d : 0.0f; }

void gfx_texrect(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
    uint32_t cyc = (g.rdp_omh >> 20) & 3;
    int flip = (w0 >> 24) == G_TEXRECTFLIP;
    float lrx = (float)((w0 >> 12) & 0xFFF) * 0.25f, lry = (float)(w0 & 0xFFF) * 0.25f;
    float ulx = (float)((w1 >> 12) & 0xFFF) * 0.25f, uly = (float)(w1 & 0xFFF) * 0.25f;
    float s = (float)(int16_t)(w2 >> 16), t = (float)(int16_t)w2;
    float dsdx = (float)(int16_t)(w3 >> 16) / 1024.0f, dtdy = (float)(int16_t)w3 / 1024.0f;
    if (cyc == CYC_COPY) dsdx *= 0.25f, lrx += 1.0f, lry += 1.0f;
    ensure_frame();
    const struct rdp_tile *tl = &g.tile[(g.rect_w1 >> 24) & 7];
    float ds = shift_st(dsdx, tl->shifts), dt = shift_st(dtdy, tl->shiftt);
    float s0 = shift_st(s, tl->shifts) - (float)(tl->uls << 3), t0 = shift_st(t, tl->shiftt) - (float)(tl->ult << 3);
    // Whole texels stepped by whole texels leave the RDP's filter fractions all zero, so it shows texels s0, s0 + ds,
    // ...: only those, point-sampled 1:1, match it once upscaled (bilinear also blends strips through their wrap)
    int exact = whole_step(ds) && whole_step(dt) && whole_texel(s0) && whole_texel(t0);
    if (exact) rect_dec = (struct tex_dec){ dec_step(ds), dec_step(dt), dec_phase(s0, ds), dec_phase(t0, dt) };
    resolve(1, 1);
    rect_dec = no_dec;
    if (exact) cur.filter = 0;
    ds /= (float)tb.step_s, dt /= (float)tb.step_t;
    rect_phase_s = tb.phase_s, rect_phase_t = tb.phase_t;
    rect_inv_step_s = 1.0f / (float)tb.step_s, rect_inv_step_t = 1.0f / (float)tb.step_t;
    rect_off_s = rect_off(ds, cur.filter || exact), rect_off_t = rect_off(dt, cur.filter || exact);
    float w = lrx - ulx, h = lry - uly;
    // When the RDP's samples never leave the tile, clamping matches it, and keeps upscaled sub-pixels off the wrap
    int n_s = ceil_pos(flip ? h : w), n_t = ceil_pos(flip ? w : h);
    if (!exact && !cur.clamp_u && !cur.flip_u && in_tile(s0 * (1.0f / 32.0f), ds, n_s, tb.w, cur.filter)) cur.clamp_u = 1;
    if (!exact && !cur.clamp_v && !cur.flip_v && in_tile(t0 * (1.0f / 32.0f), dt, n_t, tb.h, cur.filter)) cur.clamp_v = 1;
    float s1 = s + (flip ? h : w) * dsdx * 32.0f, t1 = t + (flip ? w : h) * dtdy * 32.0f;
    rect_quad(ulx, uly, lrx, lry, s, t, s1, t1, flip);
    gfx_dirty = 1;
}
