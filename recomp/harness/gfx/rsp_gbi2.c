// F3DEX2 2.04H display lists (GBI2), following Mr-Wiseguy/f3dex2's f3dex2.s, which builds Kirby 64's ucode byte for
// byte; and S2DEX2 2.04, which G_LOAD_UCODE switches to mid-list and which shares the commands it doesn't redefine
#include "rsp_gfx.h"
#include "rsp_s2dex.h"
#include "gfx.h"
#include "gfx_dl.h"
#include "f3d.h"
#include "os.h"

static struct gfx_stats *stats;
static int mats_stale;  // the float MV, P and MP are newer than DMEM's s15.16 copies

static uint32_t seg_phys(uint32_t a) { return gfx_seg_phys(X2_SEGMENTS, a); }

// The geometry word in the F3D bit layout gfx_draw.c reads, with G_TEXTURE's on byte as G_TEXTURE_ENABLE
static void geom_changed(void) {
    uint32_t m = dmem32(X2_GEOM);
    g.geom = (m & (G_ZBUFFER | G_SHADE | G_FOG | G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR))
             | (m & X2G_CULL_FRONT ? G_CULL_FRONT : 0) | (m & X2G_CULL_BACK ? G_CULL_BACK : 0)
             | (m & X2G_SHADING_SMOOTH ? G_SHADING_SMOOTH : 0) | (g.dmem[X2_TEXTURE_W0 + 3] ? G_TEXTURE_ENABLE : 0);
    gfx_state_changed();
}

static void mats_sync(void) {
    if (!mats_stale) return;
    mats_stale = 0;
    gfx_mtx_to_dmem(X2_MV, g.mv);
    gfx_mtx_to_dmem(X2_P, g.proj);
    gfx_mtx_to_dmem(X2_MVP, g.mp);
}

static int overlaps(uint32_t a, uint32_t e, uint32_t lo, uint32_t n) { return e > lo && a < lo + n; }

// A MOVEMEM or MOVEWORD wrote [a, a + n); mats_sync ran first if it touched the matrices
static void dmem_written(uint32_t a, uint32_t n) {
    uint32_t e = a + n;
    if (overlaps(a, e, X2_MV, 64)) gfx_mtx_from_dmem(X2_MV, g.mv);
    if (overlaps(a, e, X2_P, 64)) gfx_mtx_from_dmem(X2_P, g.proj);
    if (overlaps(a, e, X2_MVP, 64)) gfx_mtx_from_dmem(X2_MVP, g.mp);
    if (overlaps(a, e, X2_VIEWPORT, 16) || overlaps(a, e, X2_FOG, 4)) gfx_vp_from_dmem();
    if (overlaps(a, e, X2_GEOM, 4) || overlaps(a, e, X2_TEXTURE_W0, 8)) geom_changed();
    if (overlaps(a, e, X2_VTXBUF, GFX_MAX_VTX * X2_VTX_SIZE)) os_die("rsp gfx: F3DEX2 write into the vertex buffer", a);
}

static void invalidate_mvp_lights(void) {  // sw $zero, mvpValid: the word over mvpValid, its pad and lightsValid
    g.dmem[X2_MVP_VALID] = g.dmem[X2_MVP_VALID + 1] = g.dmem[X2_MVP_VALID + 2] = g.dmem[X2_LIGHTS_VALID] = 0;
}

// G_VTX: n << 12 | (v0 + n) << 1; MP is recomputed when mvpValid is clear
GFX_HOT(10) __attribute__((noinline)) static void vtx2(uint32_t w0, uint32_t w1) {
    uint32_t n = (w0 >> 12) & 0xFF, end = (w0 >> 1) & 0x7F, src = seg_phys(w1);
    if ((w0 & 0x00F00F01u) || n == 0 || n > end || end > GFX_MAX_VTX) os_die("rsp gfx: F3DEX2 G_VTX encoding", w0);
    gfx_need(src, n * 16, "rsp gfx: G_VTX source out of RDRAM");
    if (!g.dmem[X2_MVP_VALID]) {
        g.dmem[X2_MVP_VALID] = (uint8_t)w0;
        gfx_update_mp();
        mats_stale = 1;
    }
    gfx_load_vtx(src, n, end - n, dmem32(X2_TEXTURE_W0 + 4), (int16_t)dmem16(X2_CLIP_RATIO));
    stats->vtx += n;
}

// tri_to_rdp: index bytes are vertex * 2; flat shading takes RGB from the first index, before the y sort
GFX_HOT(10) __attribute__((noinline)) static void tri2(uint32_t w) {
    if (w & 0x00C1C1C1u) os_die("rsp gfx: F3DEX2 triangle vertex index not in 0..31 (x2)", w);
    int a = (int)(w >> 17) & 31, b = (int)(w >> 9) & 31, c = (int)(w >> 1) & 31;
    gfx_tri(a, b, c, a);
    stats->tris++;
}

// The index is a byte offset into the table; S2DEX2's starts at 0x386 and has entries 2..8 only
static void moveword(uint32_t w0, uint32_t w1, int s2) {
    uint32_t idx = (w0 >> 16) & 0xFF;
    if ((idx & 1) || idx > (s2 ? 8u : 0x0Eu) || (s2 && !idx)) os_die("rsp gfx: GBI2 G_MOVEWORD index past its table", w0);
    uint32_t a = (dmem16((s2 ? S2_MOVEWORD_TAB : X2_MOVEWORD_TAB) + idx) + w0) & 0xFFF;
    if (a < X2_MTX_END) mats_sync();
    dmem_set32(a, w1);
    if (!s2) dmem_written(a, 4);
}

// S2DEX2's commands; 0 for the ones it shares with F3DEX2
static int s2dex_cmd(uint32_t op, uint32_t w0, uint32_t w1) {
    switch (op) {
    case 0x03:  // its jump table sends it to the main loop
        return 1;
    case S2_OBJ_LOADTXTR:
        s2dex_obj_loadtxtr(seg_phys(w1));
        return 1;
    case S2_BG_1CYC:
        s2dex_bg_1cyc(seg_phys(w1), g.dmem[S2_RENDERMODE]);
        return 1;
    case S2_BG_COPY:
        s2dex_bg_copy(seg_phys(w1));
        return 1;
    case S2_OBJ_RENDERMODE:
        g.dmem[S2_RENDERMODE] = (uint8_t)w1;
        return 1;
    case S2_OBJ_MOVEMEM: {  // the table entry at 0x388 + w0's low 12 bits; no offset
        uint32_t dst = dmem16(S2_MOVEMEM_TAB + (w0 & 0xFFF)) & 0x1FF8, src = seg_phys(w1), len = (((w0 >> 16) & 0xFF) | 7) + 1;
        if (dst & 0x1000) os_die("rsp gfx: S2DEX2 G_OBJ_MOVEMEM into IMEM", w0);
        gfx_need(src, len, "rsp gfx: G_OBJ_MOVEMEM source out of RDRAM");
        gfx_dmem_load(dst, src, len);
        return 1;
    }
    case S2_OBJ_RECTANGLE:
    case S2_OBJ_SPRITE:
    case S2_SELECT_DL:
    case S2_OBJ_LDTX_SPRITE:
    case S2_OBJ_LDTX_RECT:
    case S2_OBJ_LDTX_RECT_R:
    case S2_OBJ_RECTANGLE_R:
        os_die("rsp gfx: S2DEX2 command the HLE doesn't model yet", w0);
    default:
        if (op >= 0x0C && op < X2_MOVEWORD) os_die("rsp gfx: display list command S2DEX2 doesn't have", w0);
        return 0;
    }
}

// do_movemem: idx & 0xFE picks the table entry, plus ofs * 8; the length byte is (len - 1) / 8 << 3
static void movemem(uint32_t w0, uint32_t src) {
    uint32_t idx = w0 & 0xFE, len = (((w0 >> 16) & 0xFF) | 7) + 1;
    uint32_t dst = (dmem16(X2_MOVEMEM_TAB + idx) + (w0 >> 5)) & 0x1FF8;
    if (idx > 0x1C) os_die("rsp gfx: F3DEX2 G_MOVEMEM index past its table", w0);
    if (dst & 0x1000) os_die("rsp gfx: F3DEX2 G_MOVEMEM into IMEM", w0);
    gfx_need(src, len, "rsp gfx: G_MOVEMEM source out of RDRAM");
    if (dst < X2_MTX_END) mats_sync();
    gfx_dmem_load(dst, src, len);
    dmem_written(dst, len);
}

// ovl0 after G_LOAD_UCODE: DMEM from spFxBase up is the new ucode's data; below it (matrices, viewport, segments,
// the DL stack, othermode, texrect words) carries over
static int load_ucode(uint32_t w0) {
    uint32_t data = dmem32(X2_HALF1) & 0x00FFFFFF, n = ((((w0 & 0xFFF) - X2_FXBASE) & 0xFFF) | 7) + 1;
    const struct gfx_ucode *u = gfx_ucode_find(data);
    if (u->kind < UC_F3DEX2) os_die("rsp gfx: G_LOAD_UCODE to a microcode outside the GBI2 family", data);
    gfx_need(data, X2_FXBASE + n, "rsp gfx: G_LOAD_UCODE data out of RDRAM");
    gfx_dmem_load(X2_FXBASE, data + X2_FXBASE, n);
    gfx_uc = u;
    if (u->kind == UC_S2DEX2) return 1;
    gfx_vp_from_dmem();
    geom_changed();
    return 0;
}

static void othermode(uint32_t *mode, uint32_t w0, uint32_t w1) {
    uint32_t mask = (uint32_t)((int32_t)0x80000000 >> (w0 & 31)) >> ((w0 >> 8) & 31);
    *mode = (*mode & ~mask) | w1;
    gfx_rdp(g.omh, g.oml);
}

GFX_HOT(10) void rsp_gbi2_task(const uint8_t *dmem, struct gfx_stats *st) {
    stats = st;
    g.omh = dmem32(X2_OTHERMODE);
    g.oml = dmem32(X2_OTHERMODE + 4);
    g.mv_depth = 0;
    g.mv_max = MV_STACK_CAP;
    g.noclip = 0;
    gfx_mtx_from_dmem(X2_MV, g.mv);
    gfx_mtx_from_dmem(X2_P, g.proj);
    gfx_mtx_from_dmem(X2_MVP, g.mp);
    mats_stale = 0;
    gfx_vp_from_dmem();
    gfx_vtx_reset();
    gfx_frame_begin();
    geom_changed();
    int s2 = gfx_uc->kind == UC_S2DEX2;
    struct dl d;
    dl_start(&d, *(const uint32_t *)(dmem + TASK_DATA_PTR) & 0x00FFFFFF, st->hash);
    for (;;) {
        uint32_t w0, w1, op = dl_next(&d, &w0, &w1);
        if (s2 && s2dex_cmd(op, w0, w1)) continue;
        switch (op) {
        case 0x00:  // G_NOOP_handler: the word goes to the RDP, which does nothing with it
            d.rdp++;
            break;
        case X2_VTX:
            vtx2(w0, w1);
            break;
        case X2_TRI1:
            tri2(w0);
            break;
        case X2_TRI2:
        case X2_QUAD:  // the w1 triangle first: G_TRI2_handler jals tri_to_rdp with it, then falls into G_TRI1
            tri2(w1);
            tri2(w0);
            break;
        case X2_MODIFYVTX: {
            uint32_t i = w0 & 0xFFFF;
            if ((i & 1) || i >= 2 * GFX_MAX_VTX) os_die("rsp gfx: F3DEX2 G_MODIFYVTX vertex not in 0..31 (x2)", w0);
            gfx_modify_vtx((i >> 1) * 40 + ((w0 >> 16) & 0xFF), w1);
            break;
        }
        case X2_CULLDL: {  // vertices first..last inclusive; all outside one plane ends the list
            uint32_t a = w0 & 0xFFFF, b = w1 & 0xFFFF;
            if ((a | b) & 1 || a > b || b >= 2 * GFX_MAX_VTX) os_die("rsp gfx: F3DEX2 G_CULLDL vertex range", w0);
            if (gfx_cull_dl(a >> 1, (b >> 1) + 1)) goto enddl;
            break;
        }
        case X2_BRANCH_Z: {  // to RDPHALF_1's list, without a push, if the vertex's s15.16 screen z is below w1
            uint32_t i = w0 & 0xFFF;
            if ((i & 1) || i >= 2 * GFX_MAX_VTX) os_die("rsp gfx: F3DEX2 G_BRANCH_Z vertex not in 0..31 (x2)", w0);
            gfx_vtx_need(&g.v[i >> 1]);
            uint32_t z = (uint32_t)(int32_t)(g.v[i >> 1].sz * (4.0f * 65536.0f));
            if ((int32_t)(z - w1) < 0) d.pc = seg_phys(dmem32(X2_HALF1));
            break;
        }
        case X2_TEXTURE:
            dmem_set32(X2_TEXTURE_W0, w0);
            dmem_set32(X2_TEXTURE_W0 + 4, w1);
            geom_changed();
            break;
        case X2_POPMTX:  // w1 bytes off the stack pointer; nothing moves, nothing goes stale
            if (w1 & 63) os_die("rsp gfx: F3DEX2 G_POPMTX by a part of a matrix", w1);
            if (gfx_pop_mtx2(w1 >> 6)) invalidate_mvp_lights(), mats_stale = 1;
            break;
        case X2_GEOMETRYMODE:
            dmem_set32(X2_GEOM, (dmem32(X2_GEOM) & w0) | w1);
            geom_changed();
            break;
        case X2_MTX: {  // flags ^ G_MTX_PUSH in the index byte: it picks the DMA target, the temp matrix or MV/P
            uint32_t src = seg_phys(w1);
            if (((((w0 >> 16) & 0xFF) | 7) + 1) != 64 || (w0 & 0xFF00)) os_die("rsp gfx: F3DEX2 G_MTX length or offset", w0);
            gfx_need(src, 64, "rsp gfx: G_MTX source out of RDRAM");
            gfx_load_mtx2(src, (w0 & X2_MTX_PROJECTION) != 0, (w0 & X2_MTX_LOAD) != 0,
                          !(w0 & (X2_MTX_PROJECTION | X2_MTX_NOPUSH)));
            invalidate_mvp_lights();
            mats_stale = 1;
            break;
        }
        case X2_MOVEWORD:
            moveword(w0, w1, s2);
            break;
        case X2_MOVEMEM:
            movemem(w0, seg_phys(w1));
            break;
        case X2_LOAD_UCODE:
            s2 = load_ucode(w0);
            break;
        case X2_DL:  // the 0x48-byte stack at 0x138, below spFxBase
            if (!dl_call(&d, st, seg_phys(w1), !((w0 >> 16) & 1), DL_DEPTH_MAX))
                os_die("rsp gfx: F3DEX2 display list stack past its 18 entries", w0);
            break;
        case X2_ENDDL:
        enddl:
            if (dl_return(&d, st)) return;
            break;
        case X2_SPECIAL_1:  // 2.04H's G_SPECIAL_1_handler: MP now, mvpValid from w0's low byte
            if (!gfx_uc->special1_mvp) break;
            g.dmem[X2_MVP_VALID] = (uint8_t)w0;
            gfx_update_mp();
            mats_stale = 1;
            break;
        case X2_SPNOOP:
        case X2_SPECIAL_2:
        case X2_SPECIAL_3:
        case X2_LINE3D:  // F3DEX2's jump table sends it to the main loop
            break;
        case X2_RDPHALF_1:
            dmem_set32(X2_HALF1, w1);
            break;
        case X2_SETOTHERMODE_H:
            othermode(&g.omh, w0, w1);
            d.rdp++;
            break;
        case X2_SETOTHERMODE_L:
            othermode(&g.oml, w0, w1);
            d.rdp++;
            break;
        case G_TEXRECT:
        case G_TEXRECTFLIP:  // kept until RDPHALF_2 sends all four words
            dmem_set32(X2_TEXRECT, w0);
            dmem_set32(X2_TEXRECT + 4, w1);
            break;
        case X2_RDPHALF_2:
            gfx_rdp(dmem32(X2_TEXRECT), dmem32(X2_TEXRECT + 4));
            gfx_rdp(dmem32(X2_HALF1), w1);
            d.rdp++;
            break;
        case G_RDPSETOTHERMODE:
            g.omh = w0, g.oml = w1;
            gfx_rdp(w0, w1);
            d.rdp++;
            break;
        case G_RDPFULLSYNC:
            st->fullsync = 1;
            // fall through
        case G_RDPLOADSYNC:
        case G_RDPPIPESYNC:
        case G_RDPTILESYNC:
            d.rdp++;
            break;
        case G_SETTIMG:
        case G_SETZIMG:
        case G_SETCIMG:
            gfx_rdp(w0, seg_phys(w1));
            d.rdp++;
            break;
        default:
            if (op == G_NOOP || (op >= G_SETKEYGB && op <= G_SETCOMBINE && op != X2_RDPHALF_2)) {
                gfx_rdp(w0, w1);
                d.rdp++;
                break;
            }
            os_die("rsp gfx: display list command F3DEX2 doesn't have", w0);
        }
    }
}
