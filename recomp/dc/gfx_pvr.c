// The graphics HLE's PVR backend: OP streams through the store queues; PT and TR are buffered and sent at frame end.
#include <kos.h>
#include <dc/pvr.h>
#include <arch/dmac.h>
#include "htypes.h"
#include "gfx_sink.h"
#include "gfx_pvr.h"

#define MAX_TEX 1024
#define BUF_ENTRIES 16384  // 32-byte headers and vertices per deferred list
#define PT_ALPHA_REF_REG ((volatile uint32_t *)0xA05F811Cu)
#define PT_ALPHA_REF 0x80
#define FB_W 640
#define FB_H 480

extern uint32_t t_rdram_bias;
static uint32_t rb_cimg;  // the color image of the frame now rendering, to read back once it's done; 0 for none
static int rb_w, rb_h;

struct tex { pvr_ptr_t ptr; uint16_t w, h; uint8_t fmt; };
struct dlist { uint32_t (*buf)[8]; uint32_t n; };

static struct tex texs[MAX_TEX + 1];
static uint32_t pt_buf[BUF_ENTRIES][8] __attribute__((aligned(32)));
static uint32_t tr_buf[BUF_ENTRIES][8] __attribute__((aligned(32)));
static struct dlist pt = { pt_buf, 0 }, tr = { tr_buf, 0 };
static uint32_t spread[1024];  // i's bits moved to the even positions
static pvr_dr_state_t dr;
static int list, fog_set;
#define MAX_UNDER 256
static uint32_t under[MAX_UNDER], n_under;  // tr_buf headers of underlays no later OP/PT poly has written depth over
static float sx, sy;
struct gfx_pvr_stats gfx_pvr_stats;

void gfx_pvr_init(void) {
    pvr_init_params_t params = {
        .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16 },
        .vertex_buf_size = 512 * 1024,
        .dma_enabled = 0,
        .fsaa_enabled = 0,
        .autosort_disabled = 1,  // the N64 blends in submission order
        .opb_overflow_count = 3,
    };
    for (uint32_t i = 0; i < 1024; i++)
        for (uint32_t b = 0; b < 10; b++) spread[i] |= ((i >> b) & 1) << (2 * b);
    vid_set_mode(DM_640x480, PM_RGB565);
    pvr_init(&params);
    pvr_set_zclip(1e-7f);  // depth is 1/w; the default background plane hides w > 10000 (daedalus main.cpp)
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    *PT_ALPHA_REF_REG = PT_ALPHA_REF;
}

static uint32_t drop_line[8] __attribute__((aligned(32)));

// An entry's 32 bytes start with w0: OP in a store queue, PT/TR in their buffer, its line allocated without a fetch
static inline __attribute__((always_inline)) uint32_t *entry(uint32_t w0) {
    gfx_pvr_stats.entries[list]++;
    if (list == GL_OP) {
        uint32_t *d = (uint32_t *)pvr_dr_target(dr);
        d[0] = w0;
        return d;
    }
    struct dlist *l = list == GL_PT ? &pt : &tr;
    if (l->n == BUF_ENTRIES) {
        gfx_pvr_stats.dropped++;
        return drop_line;
    }
    uint32_t *d = l->buf[l->n++];
    __asm__ volatile("movca.l %1,@%0" : : "r"(d), "z"(w0) : "memory");
    return d;
}

static inline __attribute__((always_inline)) void entry_done(uint32_t *d) {
    if (list == GL_OP) pvr_dr_commit(d);
}

static void put(const void *e) {
    const uint32_t *s = (const uint32_t *)e;
    uint32_t *d = entry(s[0]);
    for (int i = 1; i < 8; i++) d[i] = s[i];
    entry_done(d);
}

static inline uint32_t to5551(uint32_t p) { return (p & 0xFFC0u) | (p & 0x1Fu) << 1 | 1; }  // RGB565: g keeps its top 5

// The finished frame goes back into RDRAM at its color image, where games read it. A VRAM row holds the bus ~57 us,
// so rows move only while the game idles: a DMA each, round-robin
static uint16_t rb_row[FB_W] __attribute__((aligned(32)));
static volatile int rb_inflight;
static int rb_dead;
static uint32_t rb_y, rb_left, rb_n, rb_pairs, rb_src, rb_step_x, rb_step_y, *rb_dst;  // rb_y carries across frames
static uint32_t rb_rows;  // the finished frame's scissor bottom: a color image may be shorter than 4:3

// What readback last left in each row of recent color images. Rows land after the frame, so a row the game has
// changed since is no longer framebuffer (THPS2 frees its menu buffers and loads a level there): it is skipped
#define RB_IMGS 4
static struct rb_img { uint32_t cimg, w, used; uint32_t hash[FB_H]; uint8_t known[FB_H]; } rb_imgs[RB_IMGS], *rb_img;
static uint32_t rb_clock;

static uint32_t row_hash(const uint32_t *d, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ d[i]) * 16777619u;
    return h;
}

static struct rb_img *rb_img_of(uint32_t cimg, uint32_t w) {
    struct rb_img *m = &rb_imgs[0];
    for (int i = 0; i < RB_IMGS; i++) {
        if (rb_imgs[i].cimg == cimg && rb_imgs[i].w == w) {
            m = &rb_imgs[i];
            m->used = ++rb_clock;
            return m;
        }
        if (rb_imgs[i].used < m->used) m = &rb_imgs[i];
    }
    m->cimg = cimg, m->w = w, m->used = ++rb_clock;
    __builtin_memset(m->known, 0, sizeof m->known);
    return m;
}

static void rb_done(void *data) {
    (void)data;
    uint32_t *d = rb_dst + rb_y * rb_pairs;  // RDRAM words hold their two halves big-endian
    uint32_t now = row_hash(d, rb_pairs);
    if (rb_img->known[rb_y] && now != rb_img->hash[rb_y]) {
        rb_img->hash[rb_y] = now;
        gfx_pvr_stats.readback_skipped++;
    } else {
        for (uint32_t x = 0; x < rb_pairs * 2; x += 2) d[x / 2] = to5551(rb_row[x * rb_step_x]) << 16 | to5551(rb_row[(x + 1) * rb_step_x]);
        rb_img->hash[rb_y] = row_hash(d, rb_pairs), rb_img->known[rb_y] = 1;
    }
    rb_y = rb_y + 1 < rb_n ? rb_y + 1 : 0;
    rb_left--;
    gfx_pvr_stats.readback_rows++;
    rb_inflight = 0;
}

static const dma_config_t rb_dma = {
    .channel = DMA_CHANNEL_3, .request = DMA_REQUEST_AUTO_MEM_TO_MEM, .unit_size = DMA_UNITSIZE_32BYTE,
    .src_mode = DMA_ADDRMODE_INCREMENT, .dst_mode = DMA_ADDRMODE_INCREMENT,
    .transmit_mode = DMA_TRANSMITMODE_CYCLE_STEAL, .callback = rb_done,
};

int gfx_pvr_idle(void) {
    if (rb_inflight) return 1;
    if (!rb_left) return 0;
    rb_inflight = 1;
    dma_transfer(&rb_dma, dma_map_dst(rb_row, sizeof rb_row), hw_to_dma_addr(rb_src + rb_y * rb_step_y * sizeof rb_row),
                 sizeof rb_row, NULL);
    return 1;
}

uint32_t gfx_pvr_front(void) {
    uint32_t front = (uint32_t)pvr_get_front_buffer();
    return PVR_RAM_BASE + ((front - PVR_RAM_BASE) >> 1);  // undo its 64-bit view: frames are linear in the 32-bit one
}

// At frame begin: the row in flight lands before the PVR may render over its buffer, then rows come from the new
// front buffer; a DMA that never completes turns readback off
static void readback_retarget(void) {
    uint64_t t0 = timer_us_gettime64();
    while (rb_inflight) {
        if (timer_us_gettime64() - t0 > 5000) {
            dma_transfer_abort(DMA_CHANNEL_3);
            rb_inflight = 0, rb_dead = 1;
            printf("pvr: framebuffer readback DMA stalled at row %lu; readback off\n", (unsigned long)rb_y);
        }
    }
    rb_left = 0;
    if (!rb_cimg || rb_dead) return;
    rb_src = gfx_pvr_front();
    rb_step_x = FB_W / (uint32_t)rb_w, rb_step_y = FB_H / (uint32_t)rb_h, rb_pairs = (uint32_t)rb_w / 2;
    uint32_t n = rb_rows < (uint32_t)rb_h ? rb_rows : (uint32_t)rb_h;
    if (!n) return;
    if (rb_n != n || rb_y >= n) rb_y = 0;
    rb_n = rb_left = n;
    rb_dst = (uint32_t *)(0x80000000u + rb_cimg + t_rdram_bias);
    rb_img = rb_img_of(rb_cimg, (uint32_t)rb_w);
}

#define DRAWN_N 4  // a triple-buffered swap chain plus a render target
static struct { uint32_t lo, hi; } drawn_imgs[DRAWN_N];  // most recent first

static void note_drawn(uint32_t cimg, uint32_t bytes) {
    int i = 0;
    while (i < DRAWN_N - 1 && drawn_imgs[i].lo != cimg) i++;
    for (; i > 0; i--) drawn_imgs[i] = drawn_imgs[i - 1];
    drawn_imgs[0].lo = cimg, drawn_imgs[0].hi = cimg + bytes;
}

int gfx_pvr_drew(uint32_t phys) {
    for (int i = 0; i < DRAWN_N; i++)
        if (phys >= drawn_imgs[i].lo && phys < drawn_imgs[i].hi) return 1;
    return 0;
}

static int scene_open;
static pvr_ptr_t cpu_tex;  // 512x256 RGB565, not twiddled: a framebuffer the CPU drew, or a kept frame
static int cpu_w, cpu_h;   // the part of it that holds the image
static int frame_w, frame_h, held;
static struct gfx_cover prev_drawn, held_drawn;  // the last frame's drawn cells, and the cells the kept frame stands in for

// cpu_tex over the whole screen, depth untouched
static void cpu_quad(pvr_list_t which, int cmp, uint32_t out[5][8]) {
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, which, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, 512, 256, cpu_tex, PVR_FILTER_NONE);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.blend.src = PVR_BLEND_ONE, cxt.blend.dst = PVR_BLEND_ZERO;
    cxt.depth.comparison = cmp;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_poly_compile((pvr_poly_hdr_t *)out[0], &cxt);
    float u = (float)cpu_w / 512.0f, v = (float)cpu_h / 256.0f, z = 2e-7f;  // just above the 1e-7 clear plane
    pvr_vertex_t *q = (pvr_vertex_t *)out[1];
    q[0] = (pvr_vertex_t){ PVR_CMD_VERTEX, 0.0f, 0.0f, z, 0.0f, 0.0f, 0xFFFFFFFFu, 0 };
    q[1] = (pvr_vertex_t){ PVR_CMD_VERTEX, 640.0f, 0.0f, z, u, 0.0f, 0xFFFFFFFFu, 0 };
    q[2] = (pvr_vertex_t){ PVR_CMD_VERTEX, 0.0f, 480.0f, z, 0.0f, v, 0xFFFFFFFFu, 0 };
    q[3] = (pvr_vertex_t){ PVR_CMD_VERTEX_EOL, 640.0f, 480.0f, z, u, v, 0xFFFFFFFFu, 0 };
}

static int cover_has(const struct gfx_cover *a, const struct gfx_cover *b) {
    for (int r = 0; r < GFX_COVER_ROWS; r++)
        if (b->row[r] & ~a->row[r]) return 0;
    return 1;
}

#define CELL_WORDS (160 / GFX_COVER_COLS)  // a cell's width in cpu_tex words at 320x240
#define CELL_ROWS (240 / GFX_COVER_ROWS)

// The frame on screen at 320x240 into cpu_tex, all of it or only some cells: what an N64 color image still holds
// where a frame doesn't draw. About 14 ms of VRAM reads for all of it
static int keep_frame(const struct gfx_cover *cells) {
    if (!cpu_tex && !(cpu_tex = pvr_mem_malloc(512 * 256 * 2))) return 0;
    pvr_wait_render_done();
    uint32_t front = (uint32_t)pvr_get_front_buffer();
    const volatile uint32_t *src = (const volatile uint32_t *)(PVR_RAM_BASE + ((front - PVR_RAM_BASE) >> 1));
    for (int y = 0; y < 240; y++, src += FB_W) {  // every other row and column
        uint32_t m = cells ? cells->row[y / CELL_ROWS] : ~0u;
        volatile uint32_t *d = (volatile uint32_t *)cpu_tex + y * 256;
        for (int c = 0; m; c++, m >>= 1)
            if (m & 1)
                for (int x = c * CELL_WORDS; x < (c + 1) * CELL_WORDS; x++) d[x] = (src[2 * x] & 0xFFFFu) | src[2 * x + 1] << 16;
    }
    cpu_w = 320, cpu_h = 240;
    gfx_pvr_stats.kept_frames++;
    return 1;
}

int gfx_pvr_show_rdram(uint32_t phys, int w, int h) {
    if (scene_open || w <= 0 || w > 512 || (w & 1) || h <= 0 || h > 256 || phys + (uint32_t)(w * h * 2) > RDRAM_BYTES) return 0;
    if (!cpu_tex && !(cpu_tex = pvr_mem_malloc(512 * 256 * 2))) return 0;
    uint64_t t0 = timer_us_gettime64();
    pvr_wait_ready();
    gfx_pvr_stats.wait_us += timer_us_gettime64() - t0;
    rb_cimg = 0;
    readback_retarget();
    held = 0, prev_drawn = (struct gfx_cover){ { 0 } };
    cpu_w = w, cpu_h = h;
    const uint32_t *src = (const uint32_t *)(0x80000000u + phys + t_rdram_bias);
    for (int y = 0; y < h; y++) {
        volatile uint32_t *d = (volatile uint32_t *)cpu_tex + y * 256;
        for (int x = 0; x < w / 2; x++) {  // RDRAM words hold their two RGBA5551 halves big-endian
            uint32_t p = *src++, a = p >> 16, b = p & 0xFFFF;
            a = (a & 0xFFC0u) | (a >> 1 & 0x1Fu) | (a >> 5 & 0x20u), b = (b & 0xFFC0u) | (b >> 1 & 0x1Fu) | (b >> 5 & 0x20u);
            d[x] = a | b << 16;
        }
    }
    static uint32_t q[5][8] __attribute__((aligned(32)));
    cpu_quad(PVR_LIST_OP_POLY, PVR_DEPTHCMP_ALWAYS, q);
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(q, sizeof q);
    pvr_list_finish();
    pvr_scene_finish();
    return 1;
}

static void p_frame_begin(uint32_t cimg, int siz, int w, int h) {
    scene_open = 1;
    if (siz >= 2 && w > 0 && h > 0) note_drawn(cimg, (uint32_t)(w * h) << (siz - 1));
    sx = 640.0f / (float)w, sy = 480.0f / (float)h;
    pt.n = tr.n = 0;
    fog_set = 0, n_under = 0;
    uint64_t t0 = timer_us_gettime64();
    pvr_wait_ready();
    gfx_pvr_stats.wait_us += timer_us_gettime64() - t0;
    readback_retarget();
    int fits = siz == 2 && w > 0 && h > 0 && w <= FB_W && h <= FB_H && !(FB_W % w) && !(FB_H % h) && !(w & 1)
               && cimg + (uint32_t)(w * h * 2) <= RDRAM_BYTES;
    rb_cimg = fits ? cimg : 0, rb_w = w, rb_h = h, rb_rows = 0;
    if (w != frame_w || h != frame_h) held = 0, prev_drawn = (struct gfx_cover){ { 0 } };
    frame_w = w, frame_h = h;
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_dr_init(&dr);
    list = GL_OP;
    if (held) {  // under everything the frame draws, as the old pixels are on the N64
        uint32_t q[5][8] __attribute__((aligned(32)));
        cpu_quad(PVR_LIST_OP_POLY, PVR_DEPTHCMP_ALWAYS, q);
        for (int i = 0; i < 5; i++) put(q[i]);
    }
}

static void p_poly(const struct gfx_poly *p) {
    static const int lists[3] = { PVR_LIST_OP_POLY, PVR_LIST_PT_POLY, PVR_LIST_TR_POLY };
    pvr_poly_cxt_t cxt;
    const struct tex *t = p->textured ? &texs[p->tex_id] : 0;
    if (t && t->ptr)
        pvr_poly_cxt_txr(&cxt, lists[p->list], (int)t->fmt << 27 | PVR_TXRFMT_TWIDDLED, t->w, t->h, t->ptr,
                         p->filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
    else
        pvr_poly_cxt_col(&cxt, lists[p->list]);
    cxt.gen.alpha = p->list == GL_OP ? PVR_ALPHA_DISABLE : PVR_ALPHA_ENABLE;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.specular = (p->offset || p->fog) ? PVR_SPECULAR_ENABLE : PVR_SPECULAR_DISABLE;
    cxt.gen.fog_type = p->fog ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
    cxt.blend.src = p->src;
    cxt.blend.dst = p->dst;
    cxt.depth.comparison = p->depth_cmp;
    cxt.depth.write = p->depth_write ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;
    if (t && t->ptr) {
        cxt.txr.env = p->txr_env;
        cxt.txr.alpha = p->ignore_tex_alpha ? PVR_TXRALPHA_DISABLE : PVR_TXRALPHA_ENABLE;
        cxt.txr.uv_clamp = (p->clamp_u ? PVR_UVCLAMP_U : 0) | (p->clamp_v ? PVR_UVCLAMP_V : 0);
        cxt.txr.uv_flip = (p->flip_u ? PVR_UVFLIP_U : 0) | (p->flip_v ? PVR_UVFLIP_V : 0);
    } else if (p->textured) {
        gfx_pvr_stats.missing_tex++;
    }
    if (p->fog && !fog_set) {  // vertex fog reads FOG_COL_VERT, not the table color; N64 fog is RGBA8888
        PVR_SET(PVR_FOG_VERTEX_COLOR, p->fog_argb >> 8);
        fog_set = 1;
    }
    pvr_poly_hdr_t hdr;
    pvr_poly_compile(&hdr, &cxt);
    list = p->list;
    uint32_t at = tr.n;
    put(&hdr);
    if (p->list == GL_TR && p->underlay && tr.n > at && n_under < MAX_UNDER) under[n_under++] = at;
    else if (p->list == GL_PT || (p->list == GL_OP && p->depth_write)) n_under = 0;
    gfx_pvr_stats.headers++;
}

static inline __attribute__((always_inline)) void put_vtx(uint32_t flags, const struct gfx_pv *v) {
    uint32_t *d = entry(flags);
    float *f = (float *)d;
    f[1] = v->x * sx, f[2] = v->y * sy, f[3] = v->z, f[4] = v->u, f[5] = v->v;
    d[6] = v->argb, d[7] = v->oargb;
    entry_done(d);
}

__attribute__((section(".text.gfxhot.08"))) static void p_tri(const struct gfx_pv *a, const struct gfx_pv *b,
                                                               const struct gfx_pv *c) {
    const struct gfx_pv *v[3] = { a, b, c };
#pragma GCC unroll 1
    for (int i = 0; i < 3; i++) put_vtx(i == 2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, v[i]);
    gfx_pvr_stats.tris++;
}

// KOS's twiddled layout (pvr_txr_load_ex) without its two divides a texel: min x min squares along the long side,
// each in Morton order with y in the low bit, so rows y and y + 1 pair up into one 32-bit store
static void twiddle(uint32_t *dst, const uint16_t *src, uint32_t w, uint32_t h) {
    uint32_t min = w < h ? w : h, mask = min - 1, lmin = (uint32_t)__builtin_ctz(min);
    for (uint32_t y = 0; y < h; y += 2) {
        const uint16_t *r0 = src + y * w, *r1 = r0 + w;
        uint32_t ty = spread[y & mask] + ((y >> lmin) << (2 * lmin));
        for (uint32_t x = 0; x < w; x++) {
            uint32_t o = ty + (spread[x & mask] << 1) + ((x >> lmin) << (2 * lmin));
            dst[o >> 1] = r0[x] | (uint32_t)r1[x] << 16;
        }
    }
}

static void p_tex_upload(uint16_t id, int w, int h, int fmt, const uint16_t *texels) {
    if (id > MAX_TEX) return;
    struct tex *t = &texs[id];
    if (t->ptr && (t->w != w || t->h != h)) {
        pvr_mem_free(t->ptr);
        t->ptr = 0;
    }
    if (!t->ptr) t->ptr = pvr_mem_malloc((size_t)(w * h * 2));
    if (!t->ptr) {
        gfx_pvr_stats.vram_full++;
        return;
    }
    t->w = (uint16_t)w, t->h = (uint16_t)h, t->fmt = (uint8_t)fmt;
    twiddle((uint32_t *)t->ptr, texels, (uint32_t)w, (uint32_t)h);
    gfx_pvr_stats.uploads++;
}

static void flush(struct dlist *l, pvr_list_t which) {
    if (!l->n) return;
    pvr_list_begin(which);
    pvr_prim(l->buf, l->n * 32);
    pvr_list_finish();
}

// A frame that draws less than the one before leaves the rest of its color image as it was: keep the last frame
// under the frames after it until one draws as much again. Once kept, only the cells just left undrawn change
static int hold_last_frame(const struct gfx_cover *d) {
    int fresh = 0;
    if (!cover_has(d, &prev_drawn)) {
        struct gfx_cover gone;
        for (int r = 0; r < GFX_COVER_ROWS; r++) gone.row[r] = prev_drawn.row[r] & ~d->row[r];
        if (keep_frame(held ? &gone : 0)) {
            for (int r = 0; r < GFX_COVER_ROWS; r++) held_drawn.row[r] = (held ? held_drawn.row[r] : 0) | prev_drawn.row[r];
            held = fresh = 1;
        }
    } else if (held && cover_has(d, &held_drawn)) {
        held = 0;
    }
    prev_drawn = *d;
    return fresh;
}

static void p_frame_end(const struct gfx_cover *drawn, int rows) {
    rb_rows = rows > 0 ? (uint32_t)rows : 0;
    pvr_list_finish();
    int fresh = hold_last_frame(drawn);
    for (uint32_t i = 0; i < n_under; i++) tr.buf[under[i]][1] |= (uint32_t)PVR_DEPTHCMP_ALWAYS << 29;  // mode1 31:29
    flush(&pt, PVR_LIST_PT_POLY);  // PT first: cutouts write depth before TR composites over them
    if (fresh) {  // this frame's OP list has gone: fill what nothing wrote depth over, first in TR
        static uint32_t q[5][8] __attribute__((aligned(32)));
        cpu_quad(PVR_LIST_TR_POLY, PVR_DEPTHCMP_GREATER, q);
        pvr_list_begin(PVR_LIST_TR_POLY);
        pvr_prim(q, sizeof q);
        if (tr.n) pvr_prim(tr.buf, tr.n * 32);
        pvr_list_finish();
    } else {
        flush(&tr, PVR_LIST_TR_POLY);
    }
    pvr_scene_finish();
    scene_open = 0;
}

const struct gfx_sink gfx_sink_pvr = { p_frame_begin, p_poly, p_tri, p_tex_upload, p_frame_end };
