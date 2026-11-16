// RSP graphics tasks: Fast3D display lists run on the CPU, following rsp/fast3d.s's dispatch and DMEM layout.
#include "rsp_gfx.h"
#include "gfx.h"
#include "gfx_dl.h"
#include "f3d.h"
#include "os.h"

#define DMEM_TABLE 0x270u     // dmemTableOffset: MOVEMEM index 0x80.. -> DMEM address (both ucodes)
#define MOVEWORD_TABLE 0x30Eu  // MOVEWORD index -> DMEM base (both ucodes)
#define DMEM_SEGMENTS 0x160u
#define DMEM_CLIP_RATIO 0x76u
#define MV_STACK_F3D 10  // fast3d.s: the DRAM stack spans 0x280 bytes

struct gfx_state g;
static struct gfx_stats *stats;
void (*rsp_gfx_poll)(void);

// dl_depth: a push with that many return addresses already stacked is dropped (fast3d.s dma_DL; F3DEX at 0x1768)
#define F3D_FAMILY .texture = DMEM_STATE + 0x10, .clip_ratio = 0x76, .vp_sign = 0x50, .fog_min = 1
#define F3DEX_095(name) .version = "RSP Gfx ucode " name " Yoshitaka Yasumoto Nintendo.", .version_off = 0x2B0, \
    .viewport = 0x380, .fog = 0x390, .mp = 0x460, .dl_depth = 18, .nvtx = 32, F3D_FAMILY
#define F3DEX2_LAYOUT .viewport = X2_VIEWPORT, .fog = X2_FOG, .mp = X2_MVP, .dl_depth = 18, .cull_mask = 0x7070, \
    .texture = X2_TEXTURE_W0, .clip_ratio = X2_CLIP_RATIO, .kind = UC_F3DEX2, .nvtx = 32, .flat_rgb = 1
#define F3DEX2_204(ver) .version = "RSP Gfx ucode F3DEX       fifo " ver " Yoshitaka Yasumoto 1998 Nintendo.", \
    .version_off = 0x139, F3DEX2_LAYOUT
static const struct gfx_ucode ucodes[] = {
    { .version = "RSP SW Version: 2.0D, 04-01-96", .version_off = 0x270, .viewport = 0x320, .fog = 0x330, .mp = 0x3E0,
      .dl_depth = 10, .cull_mask = 0xFFFF, .kind = UC_F3D, .nvtx = 16, F3D_FAMILY },
    { F3DEX_095("F3DEX         0.95"), .cull_mask = 0x7070, .kind = UC_F3DEX },
    { F3DEX_095("F3DLX         0.95"), .cull_mask = 0x7070, .kind = UC_F3DLX },
    // Rayman 2: 0.95's DMEM layout but MOVEWORD PERSPNORM at 0x110, no near plane in CULLDL; its handlers differ (below)
    { F3DEX_095("F3DEX.NoN     1.23"), .cull_mask = 0x7030, .kind = UC_F3DEX, .f3dex12 = 1, .no_near = 1 },
    // Elmo's Number Journey: NoN 1.23's dispatch and DMEM, with the near plane (0x7070 where NoN has 0x7030)
    { F3DEX_095("F3DEX         1.23"), .cull_mask = 0x7070, .kind = UC_F3DEX, .f3dex12 = 1 },
    // Kirby 64 (rsp_gbi2.c): the ID string follows an extra 0x0A at 0x138; DL depth is the 0x48-byte stack's
    { F3DEX2_204("2.04H"), .special1_mvp = 1 },
    // Cruis'n World: Mr-Wiseguy/f3dex2's F3DEX2_2.04 byte for byte; only G_SPECIAL_1 differs from 2.04H
    { F3DEX2_204("2.04 ") },
    // Pokemon Stadium 2, Paper Mario: Mr-Wiseguy/f3dex2's F3DEX2_2.08PL, no 0x0A before the ID, none of 2.04's bugs
    { .version = "RSP Gfx ucode F3DEX       fifo 2.08  Yoshitaka Yasumoto/Kawasedo 1999.", .version_off = 0x138,
      F3DEX2_LAYOUT, .texgen_fixed = 1, .point_lights = 1 },
    // THPS2: F3DEX2_NoN_2.08, 2.08PL less point lighting; its near clip plane is w > 0, and CULLDL still tests it
    { .version = "RSP Gfx ucode F3DEX.NoN   fifo 2.08  Yoshitaka Yasumoto 1999 Nintendo.", .version_off = 0x138,
      F3DEX2_LAYOUT, .texgen_fixed = 1, .no_near = 1 },
    // Kirby 64's 2D ucode, reached by G_LOAD_UCODE; F3DEX2's DMEM layout below 0x180 (rsp_s2dex.c)
    { .version = "RSP Gfx ucode S2DEX       fifo 2.04  Yoshitaka Yasumoto 1998 Nintendo.", .version_off = 0x139,
      .viewport = X2_VIEWPORT, .fog = X2_FOG, .mp = X2_MVP, .dl_depth = 18, .texture = X2_TEXTURE_W0,
      .clip_ratio = X2_CLIP_RATIO, .kind = UC_S2DEX2 },
};
const struct gfx_ucode *gfx_uc = &ucodes[0];

static uint32_t task_word(const uint8_t *dmem, uint32_t off) { return *(const uint32_t *)(dmem + off); }

// G_TRI1 index bytes are vertex * 10: entry + 1, so 0 marks a byte fast3d.s can't mean
static const uint8_t tri_index[256] = {
    [0] = 1, [10] = 2, [20] = 3, [30] = 4, [40] = 5, [50] = 6, [60] = 7, [70] = 8,
    [80] = 9, [90] = 10, [100] = 11, [110] = 12, [120] = 13, [130] = 14, [140] = 15, [150] = 16,
};

static uint32_t seg_phys(uint32_t a) { return gfx_seg_phys(DMEM_SEGMENTS, a); }

// The ID's characters only: F3DEX2's ends in 0x0A, and data follows it at 0x180
static int version_is(uint32_t data, const struct gfx_ucode *u) {
    for (const char *s = u->version; *s; s++)
        if (gfx_rd8(data + u->version_off + (uint32_t)(s - u->version)) != (uint8_t)*s) return 0;
    return 1;
}

const struct gfx_ucode *gfx_ucode_find(uint32_t data) {
    gfx_need(data, 0x800, "rsp gfx: ucode_data out of RDRAM");
    for (unsigned k = 0; k < sizeof ucodes / sizeof ucodes[0]; k++)
        if (version_is(data, &ucodes[k])) return &ucodes[k];
    os_die("rsp gfx: microcode is not Fast3D 2.0D (string at ucode_data+0x270), F3DEX/F3DLX 0.95, F3DEX(.NoN) 1.23 "
           "(+0x2B0), F3DEX2 2.04/2.04H or S2DEX2 2.04 (+0x139), F3DEX2 2.08PL or NoN 2.08 (+0x138)", data);
}

static void load_ucode_data(const uint8_t *dmem) {
    uint32_t data = task_word(dmem, TASK_UCODE_DATA) & 0x00FFFFFF;
    gfx_uc = gfx_ucode_find(data);
    gfx_dmem_load(0, data, 0x800);
    __builtin_memset(g.dmem + 0x800, 0, 0x800);
}

// F3DEX keeps [0x134] in [dram_stack, dram_stack + size): one matrix per 64 bytes
static int mv_stack_max(const uint8_t *dmem) {
    if (gfx_uc->kind == UC_F3D) return MV_STACK_F3D;
    uint32_t n = task_word(dmem, TASK_DRAM_STACK_SIZE) / 64;
    if (n > MV_STACK_CAP) os_die("rsp gfx: F3DEX DRAM matrix stack deeper than 16", n);
    return (int)n;
}

static void geom_changed(void) {
    g.noclip = gfx_uc->kind == UC_F3DLX && !(g.geom & G_CLIPPING);
    gfx_state_changed();
}

// MP, then the points buffer; the viewport, then fog; and the viewport's sign vector
static void dmem_written(uint32_t a, uint32_t n) {
    uint32_t e = a + n, mp = gfx_uc->mp, vp = gfx_uc->viewport;
    if (e > mp && a < mp + 0x40u + 40u * gfx_uc->nvtx) {
        if (e > mp + 0x40u) os_die("rsp gfx: write into the points buffer (G_MW_POINTS)", a);
        gfx_mp_from_dmem();
    }
    if ((e > vp && a < vp + 0x16u) || (e > gfx_uc->vp_sign && a < gfx_uc->vp_sign + 6u)) gfx_vp_from_dmem();
}

// F3DEX G_VTX: n << 10 | (16n - 1), v0 * 2 in bits 16..23
GFX_HOT(09) __attribute__((noinline)) static void vtx_ex(uint32_t w0, uint32_t w1) {  // last: outside Fast3D's span
    uint32_t n = (w0 >> 10) & 0x3F, v0 = (w0 >> 17) & 0x7F, src = seg_phys(w1);
    if ((w0 & 0x10000) || n == 0 || (w0 & 0x3FF) != 16 * n - 1) os_die("rsp gfx: F3DEX G_VTX encoding", w0);
    if (v0 + n > GFX_MAX_VTX) os_die("rsp gfx: G_VTX past the 32-entry vertex buffer", w0);
    gfx_need(src, n * 16, "rsp gfx: G_VTX source out of RDRAM");
    gfx_load_vtx(src, n, v0, dmem32(DMEM_STATE + 0x14), (int16_t)dmem16(DMEM_CLIP_RATIO));
    stats->vtx += n;
}

// F3DEX triangle bytes are vertex * 2; the ucode indexes a 32-entry table with them unchecked
GFX_HOT(09) __attribute__((noinline)) static void tri_ex(uint32_t w) {
    if (w & 0x00C1C1C1u) os_die("rsp gfx: F3DEX triangle vertex index not in 0..31 (x2)", w);
    int a = (int)(w >> 17) & 31, b = (int)(w >> 9) & 31, c = (int)(w >> 1) & 31;
    (g.noclip ? gfx_tri_noclip : gfx_tri)(a, b, c, a);
    stats->tris++;
}

// F3DEX 0x1244: split along the diagonal with the smaller screen-y span; a face-culled first half skips the second.
// The flat-shade vertex stays v0 for both halves.
GFX_COLD static void quad_ex(uint32_t w) {
    if (w & 0xC1C1C1C1u) os_die("rsp gfx: F3DEX quad vertex index not in 0..31 (x2)", w);
    int v3 = (int)(w >> 25) & 31, v0 = (int)(w >> 17) & 31, v1 = (int)(w >> 9) & 31, v2 = (int)(w >> 1) & 31;
    gfx_vtx_need(&g.v[v0]);
    gfx_vtx_need(&g.v[v1]);
    gfx_vtx_need(&g.v[v2]);
    gfx_vtx_need(&g.v[v3]);
    float d02 = g.v[v0].sy - g.v[v2].sy, d13 = g.v[v1].sy - g.v[v3].sy;
    int along02 = __builtin_fabsf(d02) <= __builtin_fabsf(d13);
    void (*tri)(int, int, int, int) = g.noclip ? gfx_tri_noclip : gfx_tri;
    uint32_t culls = gfx_rej_cull;
    tri(v0, v1, along02 ? v2 : v3, v0);
    stats->tris++;
    if (gfx_rej_cull != culls && gfx_tri_cross(v0, v1, along02 ? v2 : v3) != 0.0f) return;  // zero area isn't a cull
    tri(v2, v3, along02 ? v0 : v1, v0);
    stats->tris++;
}

static void movemem(uint32_t idx, uint32_t src) {
    uint32_t dst = dmem16(DMEM_TABLE + (idx & 0xFE)), n = 16 - (dst & 15);  // sqv stops at the 16-byte line
    gfx_need(src, n, "rsp gfx: G_MOVEMEM source out of RDRAM");
    gfx_dmem_load(dst, src, n);
    dmem_written(dst, n);
}

static void moveword(uint32_t idx, uint32_t off, uint32_t v) {
    if ((idx & 0xFF) == G_MW_POINTS) {
        gfx_modify_vtx(off, v);
        return;
    }
    uint32_t a = (dmem16(MOVEWORD_TABLE + (idx & 0xFF)) + off) & 0xFFF;
    dmem_set32(a, v);
    dmem_written(a, 4);
}

static void othermode(uint32_t *mode, uint32_t w0, uint32_t w1) {
    uint32_t len = w0 & 0xFF, shift = (w0 >> 8) & 0xFF;
    uint32_t mask = (uint32_t)(((1ull << len) - 1) << shift);
    *mode = (*mode & ~mask) | w1;
    gfx_rdp(g.omh, g.oml);
}

GFX_HOT(00) void rsp_gfx_task(const uint8_t *dmem, struct gfx_stats *st) {
    stats = st;
    __builtin_memset(st, 0, sizeof *st);
    st->hash = 0x811C9DC5u;
    load_ucode_data(dmem);
    if (gfx_uc->kind >= UC_F3DEX2) {
        rsp_gbi2_task(dmem, st);
        return;
    }
    const int ex = gfx_uc->kind != UC_F3D;
    g.geom = dmem32(DMEM_STATE + 4);
    g.omh = dmem32(DMEM_STATE + 8);
    g.oml = dmem32(DMEM_STATE + 12);
    g.mv_depth = 0;
    g.mv_max = mv_stack_max(dmem);
    __builtin_memset(g.mv, 0, sizeof g.mv);
    __builtin_memset(g.proj, 0, sizeof g.proj);
    gfx_mp_from_dmem();
    gfx_vp_from_dmem();
    gfx_vtx_reset();
    gfx_frame_begin();
    geom_changed();
    struct dl d;
    dl_start(&d, task_word(dmem, TASK_DATA_PTR) & 0x00FFFFFF, st->hash);
    for (;;) {
        uint32_t w0, w1, op = dl_next(&d, &w0, &w1);
        if (op >= 0xC0) {  // RDP commands: over half of a race frame's
            if (op >= 0xC8 && op <= 0xCF) os_die("rsp gfx: raw RDP triangle in a display list", w0);
            if (op >= G_SETTIMG) w1 = seg_phys(w1);
            if (op == G_RDPFULLSYNC) st->fullsync = 1;
            d.rdp++;
            if (op - G_RDPLOADSYNC > G_RDPTILESYNC - G_RDPLOADSYNC || g.rect_pending) gfx_rdp(w0, w1);  // syncs order nothing here
            continue;
        }
        switch (op) {
        case F3D_SPNOOP:
            break;
        case F3D_MTX:
            if ((w0 & 0xFFFF) != 64) os_die("rsp gfx: G_MTX length is not 64", w0);
            gfx_need(seg_phys(w1), 64, "rsp gfx: G_MTX source out of RDRAM");
            g.dmem[0x12C] = g.dmem[0x36];  // sbv $v31[6], numLights: lights need re-init
            gfx_load_mtx(seg_phys(w1), (w0 >> 16) & 0xFF);
            break;
        case F3D_MOVEMEM:
            movemem((w0 >> 16) & 0xFF, seg_phys(w1));
            break;
        case F3D_VTX: {
            if (ex) {
                vtx_ex(w0, w1);
                break;
            }
            uint32_t n = ((w0 >> 20) & 0xF) + 1, v0 = (w0 >> 16) & 0xF;
            if (v0 + n > 16) os_die("rsp gfx: G_VTX past the 16-entry vertex buffer", w0);
            gfx_need(seg_phys(w1), n * 16, "rsp gfx: G_VTX source out of RDRAM");
            gfx_load_vtx(seg_phys(w1), n, v0, dmem32(DMEM_STATE + 0x14), (int16_t)dmem16(DMEM_CLIP_RATIO));
            st->vtx += n;
            break;
        }
        case F3D_DL:  // a push past the ucode's depth drops the whole command
            dl_call(&d, st, seg_phys(w1), ((w0 >> 16) & 0xFF) == 0, gfx_uc->dl_depth);
            break;
        case F3D_ENDDL:
        enddl:
            if (dl_return(&d, st)) return;
            break;
        case F3D_CULLDL:
            if (gfx_cull_dl((w0 & 0x3FF) / 40, w1 / 40)) goto enddl;
            break;
        case F3D_TRI1: {
            if (ex) {  // the flag byte is ignored: the flat-shade vertex is always the first
                tri_ex(w1);
                break;
            }
            int v[3] = { tri_index[(w1 >> 16) & 0xFF] - 1, tri_index[(w1 >> 8) & 0xFF] - 1, tri_index[w1 & 0xFF] - 1 };
            uint32_t flag = w1 >> 24;
            if ((v[0] | v[1] | v[2]) < 0) os_die("rsp gfx: G_TRI1 vertex index not in 0..15 (x10)", w1);
            if (flag > 2) os_die("rsp gfx: G_TRI1 flag selects no vertex", w1);
            gfx_tri(v[0], v[1], v[2], v[flag]);
            st->tris++;
            break;
        }
        case F3DEX_TRI2:
            if (!ex) os_die("rsp gfx: display list command Fast3D doesn't have", w0);
            tri_ex(w1);  // the w1 triangle first (0x1224)
            tri_ex(w0);
            break;
        case F3D_POPMTX:
            if (ex) g.dmem[0x12C] = g.dmem[0x36];  // F3DEX also flags the lights for re-init here
            gfx_pop_mtx();
            break;
        case F3D_MOVEWORD:
            moveword(w0 & 0xFF, (w0 >> 8) & 0xFFFF, w1);
            break;
        case F3D_TEXTURE:
            dmem_set32(DMEM_STATE + 0x10, w0);
            dmem_set32(DMEM_STATE + 0x14, w1);
            g.geom = (g.geom & ~2u) | (w0 & 1) << 1;
            geom_changed();
            break;
        case F3D_SETOTHERMODE_H:
            othermode(&g.omh, w0, w1);
            break;
        case F3D_SETOTHERMODE_L:
            othermode(&g.oml, w0, w1);
            break;
        case F3D_SETGEOMETRYMODE:
            g.geom |= w1;
            geom_changed();
            break;
        case F3D_CLEARGEOMETRYMODE:
            g.geom &= ~w1;
            geom_changed();
            break;
        case F3D_RDPHALF_1:  // 1.23 (0x1340): the whole word at 0x10C, for RDPHALF_2 and BRANCH_Z
            if (gfx_uc->f3dex12) {
                dmem_set32(DMEM_STATE - 4, w1);
                break;
            }
            g.dmem[DMEM_STATE + 2] = (uint8_t)(w1 >> 8);
            g.dmem[DMEM_STATE + 3] = (uint8_t)w1;
            break;
        case F3D_RDPHALF_2:  // 1.23 (0x1348): sends [0x10C], w1 to the RDP, as 0.95's RDPHALF_CONT does
            if (gfx_uc->f3dex12) {
                gfx_rdp(dmem32(DMEM_STATE - 4), w1);
                d.rdp++;
                break;
            }
            dmem_set32(DMEM_STATE - 4, w1);
            break;
        case F3D_RDPHALF_CONT:
            if (gfx_uc->f3dex12) {  // 1.23 (0x132C): w1 into vertex (w0 & 0xFFF) / 2 at offset w0 bits 16..23
                gfx_modify_vtx(((w0 & 0xFFF) >> 1) * 40 + ((w0 >> 16) & 0xFF), w1);
                break;
            }
            gfx_rdp(dmem32(DMEM_STATE - 4), w1);
            d.rdp++;
            break;
        case F3D_LINE3D:  // fast3d.s 0xB5 is SP_NOOP; F3DEX_QUAD shares the number; 1.23's table has SP_NOOP again
            if (ex && !gfx_uc->f3dex12) quad_ex(w1);
            break;
        default:
            if (ex && gfx_uc->f3dex12 && op == F3DEX_BRANCH_Z) {  // 1.23 (0x12F0): to [0x10C], no push, if z <= w1 (s15.16)
                uint32_t i = (w0 & 0xFFF) >> 1;
                if (i >= gfx_uc->nvtx) os_die("rsp gfx: G_BRANCH_Z vertex past the buffer", w0);
                gfx_vtx_need(&g.v[i]);
                if ((int32_t)(g.v[i].sz * 65536.0f) <= (int32_t)w1) d.pc = seg_phys(dmem32(DMEM_STATE - 4));
                break;
            }
            if (ex && gfx_uc->f3dex12 && op == F3DEX_LOAD_UCODE) os_die("rsp gfx: G_LOAD_UCODE to another microcode", w0);
            if (ex && (op == F3DEX_LOAD_UCODE || op == F3DEX_BRANCH_Z)) break;  // outside the switch: one jump table
            // 1.23's tables send 0x40..0x7F and DMA ops 2, 5, 7..9 to its main loop (0x109C)
            if (gfx_uc->f3dex12 && ((op >= 0x40 && op < 0x80) || op == 2 || op == 5 || (op >= 7 && op <= 9))) break;
            os_die("rsp gfx: display list command the ucode doesn't have", w0);
        }
    }
}
