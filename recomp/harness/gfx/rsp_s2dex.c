// S2DEX2 2.04's object and background commands, as the RDP commands the ucode emits for them, through gfx_rdp
#include "rsp_s2dex.h"
#include "gfx.h"
#include "f3d.h"
#include "os.h"

static uint16_t rd16(uint32_t a) { return gfx_rd16(a); }
static uint32_t rd32(uint32_t a) { return (uint32_t)gfx_rd16(a) << 16 | gfx_rd16(a + 2); }

// G_OBJ_LOADTXTR (S2DEX2 0x1550): skipped when (status[sid] & mask) == flag, else status takes flag under mask. Type bytes:
// 1 masks twidth + 1 into SETTILE's line, 2 the size, 3 the load opcode (0x33 BLOCK, 0x34 TILE, 0x30 TLUT; 6 bits read)
void s2dex_obj_loadtxtr(uint32_t t) {
    uint32_t type = rd32(t), image = gfx_seg_phys(X2_SEGMENTS,rd32(t + 4)), tmem = rd16(t + 8), size = rd16(t + 0xA);
    uint32_t line = rd16(t + 0xC), sid = gfx_rd8(t + 0xF), flag = rd32(t + 0x10), mask = rd32(t + 0x14);
    if (sid & 0xF3) os_die("rsp gfx: S2DEX2 G_OBJ_LOADTXTR status id not 0, 4, 8 or 12", sid);
    uint32_t st = dmem32(S2_STATUS + sid);
    if ((st & mask) == flag) return;
    dmem_set32(S2_STATUS + sid, (st & ~mask) | (flag & mask));
    uint32_t op = 0xC0u | (type & 0x3F);
    if (op != G_LOADBLOCK && op != G_LOADTILE && op != G_LOADTLUT) os_die("rsp gfx: S2DEX2 G_OBJ_LOADTXTR type", type);
    uint32_t settile_line = ((size + 1) & (uint32_t)(int32_t)(int8_t)(type >> 16)) << 7;
    gfx_rdp((uint32_t)G_SETTIMG << 24 | 0x00100000u | size, image);
    gfx_rdp((uint32_t)G_SETTILE << 24 | (type >> 8 & 0xFF) << 16 | settile_line | tmem, 0x07000000u);
    gfx_rdp(op << 24, 0x07000000u | size << 14 | line);
}

// The tile 0 the strips sample: the ucode gives it size 0 and masks of 15, so s and t index TMEM directly. That
// window can't be decoded; the strip's own (its line by the rows it loaded, clamped) samples the same texels
static void strip_tile(uint32_t line_words, uint32_t siz, uint32_t rows) {
    uint32_t texels = (line_words * 16) >> siz;
    gfx_rdp((uint32_t)G_SETTILESIZE << 24, (texels - 1) << 14 | (rows - 1) << 2);
}

// G_BG_COPY's strip loop as s2dex_bg_copy leaves it, in GLideN64's names
struct bgc_strips {
    uint32_t R, S, T0, T2, imagePtr;
    uint16_t T1, A3, Q;
    int16_t U, AT;
};

// Each strip: tmemH rows loaded and copied, the rest of the frame after the line split by the image's end
static void bg_copy_strips(uint32_t bg, const struct bgc_strips *p) {
    uint16_t tmemW = rd16(bg + 0x1C), tmemH = rd16(bg + 0x1E), tmemLoadSH = rd16(bg + 0x20), tmemLoadTH = rd16(bg + 0x22);
    uint16_t tmemSizeW = rd16(bg + 0x24), tmemSize = rd16(bg + 0x26);
    uint8_t imageSiz = gfx_rd8(bg + 0x17);
    int tile_load = rd16(bg + 0x14) == G_BGLT_LOADTILE;
    uint32_t R = p->R, S = p->S, T0 = p->T0, imagePtr = p->imagePtr;
    uint16_t T1 = p->T1, A3 = p->A3;
    int16_t U = p->U, AT = p->AT;
    if (U <= 0) return;
    if (A3 > 0) U = (int16_t)(U - 4);
    if (U > AT) U = AT;
    uint32_t V = 0xE4000000u | p->T2;
    uint32_t X = ((tile_load ? ((uint32_t)p->Q << 2) - 1 : tmemLoadSH) | 0x7000) << 12;
    uint32_t Y = 0xFD100000u | (uint32_t)((tmemSizeW << 1) - 1);
    AT = (int16_t)(AT - U);
    for (int rows = U > 0;; rows = 1) {
        if (rows) {
            uint32_t Z = tile_load ? (uint32_t)tmemSize << 16 | tmemLoadSH : tmemSize, AA = X | tmemLoadTH;
            uint16_t S5 = tmemH;
            do {
                U = (int16_t)(U - S5);
                if (U < 0) {
                    Z += tmemSizeW * (uint32_t)(int32_t)U;
                    S5 = (uint16_t)(S5 + U);
                    AA = tile_load ? X | (uint32_t)(S5 - 1) : (((Z - 2) | 0xE000) << 11) | tmemLoadTH;
                }
                uint16_t BB = (uint16_t)(T1 + S5 - 1);
                gfx_rdp(Y, S);
                gfx_rdp((uint32_t)(tile_load ? G_LOADTILE : G_LOADBLOCK) << 24, AA);
                strip_tile(tmemW, imageSiz, (uint32_t)(S5 + 3) >> 2);
                gfx_rdp(V | BB, T0 | T1);
                gfx_rdp(R, 0x10000400u);
                T1 = (uint16_t)(BB + 1);
                S += Z;
            } while (U > 0);
            if (AT <= 0) return;
        }
        if (A3 > 0) {  // the line split by the image's end: its two halves through tile 6
            A3 >>= 1;
            gfx_rdp(Y, S);
            gfx_rdp(0xF5100000u, 0x06000000u);
            gfx_rdp((uint32_t)G_LOADBLOCK << 24, 0x06000000u | ((((uint32_t)((tmemSizeW >> 1) - A3) << 2) - 1) << 12));
            gfx_rdp(Y, imagePtr);
            gfx_rdp(0xF5100000u | (uint32_t)((tmemSizeW >> 1) - A3), 0x06000000u);
            gfx_rdp((uint32_t)G_LOADBLOCK << 24, 0x06000000u | ((((uint32_t)A3 << 2) - 1) << 12));
            strip_tile(tmemW, imageSiz, 1);
            gfx_rdp(V | T1, T0 | T1);
            gfx_rdp(R, 0x10000400u);
            T1 = (uint16_t)(T1 + 4);
            AT = (int16_t)(AT - 4);
            if (AT <= 0) return;
        }
        S = imagePtr + ((uint32_t)A3 << 3);
        U = AT;
        AT = 0;
    }
}

// G_BG_COPY as GLideN64's BgRectCopyStripped decodes it (its 1.7 rules): copy-mode TEXRECTs of tmemH / 4 rows each, the
// image LOADBLOCKed tmemSize bytes at a time; a line split by the image's end loads in two halves through tile 6
void s2dex_bg_copy(uint32_t bg) {
    uint16_t imageX = rd16(bg), frameW = rd16(bg + 6), imageY = rd16(bg + 8), imageH = rd16(bg + 0xA), frameH = rd16(bg + 0xE);
    int16_t frameX = (int16_t)rd16(bg + 4), frameY = (int16_t)rd16(bg + 0xC);
    uint32_t imagePtr = gfx_seg_phys(X2_SEGMENTS,rd32(bg + 0x10));
    uint16_t imageLoad = rd16(bg + 0x14), imagePal = rd16(bg + 0x18), imageFlip = rd16(bg + 0x1A);
    uint8_t imageFmt = gfx_rd8(bg + 0x16), imageSiz = gfx_rd8(bg + 0x17);
    uint16_t tmemW = rd16(bg + 0x1C), tmemH = rd16(bg + 0x1E), tmemSizeW = rd16(bg + 0x24);
    int tile_load = imageLoad == G_BGLT_LOADTILE;  // then tmemLoadSH:tmemSize is a strip's bytes (guS2DInitBg)
    if ((!tile_load && imageLoad != G_BGLT_LOADBLOCK) || imageFlip) os_die("rsp gfx: S2DEX2 G_BG_COPY load or flip", bg);
    if (imageSiz > 3 || !tmemW || tmemH < 4) os_die("rsp gfx: S2DEX2 G_BG_COPY size or TMEM layout", bg);

    int32_t t0 = frameX + frameW - g.scis_lrx, t1 = frameX - g.scis_ulx;
    int16_t Aw = (int16_t)(t0 > 0 ? t0 : 0), Bw = (int16_t)(t1 < 0 ? t1 : 0), Cw = (int16_t)(frameW + Bw - Aw);
    if (Cw <= 0) return;
    int16_t Dw = (int16_t)((((imageX * 0x2000) >> 16) & 0xFFFC) - Bw), Ew = (int16_t)(frameX - Bw);
    t0 = frameY + frameH - g.scis_lry, t1 = frameY - g.scis_uly;
    int16_t Ah = (int16_t)(t0 > 0 ? t0 : 0), Bh = (int16_t)(t1 < 0 ? t1 : 0), Ch = (int16_t)(frameH + Bh - Ah);
    if (Ch <= 0) return;
    int16_t Dh = (int16_t)((((imageY * 0x2000) >> 16) & 0xFFFC) - Bh), Eh = (int16_t)(frameY - Bh);
    int16_t F = (int16_t)(Dh - imageH), G = F >= 0 ? F : Dh, H = Dw;

    uint32_t J = (uint32_t)tmemW << 9;
    uint32_t L = ((uint32_t)((imageFmt << 2) | imageSiz) << 19) | J;
    gfx_rdp((uint32_t)G_SETTILE << 24 | 0x100000u | (tile_load ? J : 0), 0x07000000u);
    gfx_rdp((uint32_t)G_SETTILESIZE << 24, 0);
    gfx_rdp((uint32_t)G_SETTILE << 24 | L, (uint32_t)imagePal << 20 | 0x00080200u);  // the ucode's 0x0007C1F0, clamped

    static const uint16_t n0[4] = { 0x3F, 0x1F, 0x0F, 0x07 }, n1[4] = { 0x0800, 0x1000, 0x2000, 0x4000 };
    static const uint16_t n2[4] = { 0x1000, 0x2000, 0x4000, 0x8000 };
    uint32_t N0 = n0[imageSiz], N1 = n1[imageSiz], N2 = n2[imageSiz];
    G = (int16_t)((G * 0x4000) >> 16);
    uint16_t O = (uint16_t)((N0 & (uint32_t)(int32_t)H) + (uint32_t)(int32_t)Cw), Q = (uint16_t)((N2 * O) >> 16);
    uint32_t P = ((N1 * (uint32_t)(int32_t)H) >> 16) + tmemSizeW * (uint32_t)(int32_t)G;
    uint32_t R = ((N0 & (uint32_t)(int32_t)H) * 8) << 16;
    uint32_t S = ((P >> 1) << 3) + imagePtr;
    uint16_t T = (uint16_t)(Ew + Cw - 1), A1 = imageH & 0xFFFC, A2 = (uint16_t)(G << 2);
    uint16_t A3 = (uint16_t)((N1 * (uint32_t)(int32_t)H) >> 16), T1 = (uint16_t)Eh;
    uint32_t T0 = (uint32_t)(int32_t)Ew << 12, T2 = (uint32_t)T << 12;
    struct bgc_strips p = { R, S, T0, T2, imagePtr, T1, A3, Q, (int16_t)(A1 - A2), Ch };
    bg_copy_strips(bg, &p);
}

// G_BG_1CYC's state between its setup and its strips: the words the ucode keeps at DMEM 0x548-0x57C, and the rest of
// what its strip loop reads, in GLideN64's names
struct bg1 {
    uint32_t w548, w54c, w550, w564, w570, w57c;
    uint32_t imagePtr, E2_1, I2, J2, Z, R;
    int32_t F2, G2;
    uint16_t P, F1_1, s_start, dsdx, scaleH;
    uint8_t siz;
};

// The frame against the image's scaled size and the scissor: 0 if nothing of it is left. Aw/Bw/Cw and Bh are what
// the left, right and top edges lose
static int bg1_clip(uint32_t bg, int flips, uint16_t scaleW, uint16_t scaleH, int16_t *edge, struct bg1 *b) {
    uint16_t imageW = rd16(bg + 2), frameW = rd16(bg + 6), imageH = rd16(bg + 0xA), frameH = rd16(bg + 0xE);
    int16_t frameX = (int16_t)rd16(bg + 4), frameY = (int16_t)rd16(bg + 0xC);
    int32_t sc_xh = g.scis_ulx, sc_yh = g.scis_uly, sc_xl = g.scis_lrx, sc_yl = g.scis_lry;
    int16_t Aw = (int16_t)(frameW - (((((int32_t)imageW << 10) / scaleW) - 1) & 0xFFFC));
    if (Aw < 0) Aw = 0;
    if (flips) frameX = (int16_t)(frameX + Aw);
    int32_t t0 = sc_xh - frameX, t1 = frameX + frameW - sc_xl - Aw;
    int16_t Bw = (int16_t)(t0 > 0 ? t0 : 0), Cw = (int16_t)(t1 > 0 ? t1 : 0);
    if ((int16_t)frameW - Aw - Bw - Cw <= 0) return 0;
    int16_t Dw = (int16_t)(frameX + Bw), Ew = (int16_t)(frameX + frameW - Aw - Cw);
    int16_t Ah = (int16_t)(frameH - (((((int32_t)imageH << 10) / scaleH) - 1) & 0xFFFC));
    if (Ah < 0) Ah = 0;
    t0 = sc_yh - frameY, t1 = frameY + frameH - sc_yl - Ah;
    int16_t Bh = (int16_t)(t0 > 0 ? t0 : 0), Ch = (int16_t)(t1 > 0 ? t1 : 0);
    if ((int16_t)frameH - Ah - Bh - Ch <= 0) return 0;
    int16_t Dh = (int16_t)(((frameY + Bh) * 0x4000) >> 16), Eh = (int16_t)(((frameH - Ah - Bh - Ch) * 0x4000) >> 16);
    b->w548 = (uint32_t)((int32_t)Dw << 16 | Ew), b->w54c = (uint32_t)((int32_t)Dh << 16 | Eh);
    edge[0] = Aw, edge[1] = Bw, edge[2] = Cw, edge[3] = Bh;
    return 1;
}

// Part 1: where the first strip starts in the image, how many rows each strip takes, and the tiles they load through
static void bg1_setup(uint32_t bg, uint32_t rendermode, const int16_t *edge, struct bg1 *b) {
    uint16_t imageX = rd16(bg), imageW = rd16(bg + 2), frameW = rd16(bg + 6);
    uint16_t imageY = rd16(bg + 8), imageH = rd16(bg + 0xA);
    uint32_t imagePtr = gfx_seg_phys(X2_SEGMENTS,rd32(bg + 0x10));
    uint8_t imageFmt = gfx_rd8(bg + 0x16), imageSiz = gfx_rd8(bg + 0x17);
    uint16_t imagePal = rd16(bg + 0x18), scaleW = rd16(bg + 0x1C), scaleH = rd16(bg + 0x1E);
    int32_t imageYorig = (int32_t)rd32(bg + 0x20);
    int flips = rd16(bg + 0x1A) & G_BG_FLAG_FLIPS;
    int16_t Aw = edge[0], Bw = edge[1], Cw = edge[2], Bh = edge[3];
    uint16_t Fw = (uint16_t)(imageW << 3), Fh = (uint16_t)(imageH << 3);
    if (flips) Bw = Cw;  // the image starts at the right edge's clip (GLideN64's reading of the ucode)
    int16_t Gw = (int16_t)(((scaleW * Bw * 0x0200) >> 16) + imageX);
    int16_t Gh = (int16_t)(((scaleH * Bh * 0x0200) >> 16) + imageY);
    for (int32_t Hw = Gw - Fw; Hw >= 0; Hw = Gw - Fw) Gw = (int16_t)(Gw - Fw), Gh = (int16_t)(Gh + 0x20), imageYorig += 0x20;
    for (int32_t Hh = Gh - Fh; Hh >= 0; Hh = Gh - Fh) Gh = (int16_t)(Gh - Fh), imageYorig -= Fh;
    int32_t I = (int32_t)((uint32_t)((int32_t)Gh - imageYorig) << 5);
    int16_t J = (int16_t)((scaleW * (frameW - Aw - Bw - Cw)) >> 7);
    uint8_t J_2 = J + Gw + 0x0B < Fw ? 0 : 1, K = (uint8_t)((rendermode & 0x08) >> 3);

    static const uint32_t a_size[4] = { 0x01FF0080, 0x00FF0100, 0x007F0200, 0x003F0400 };
    static const uint16_t a_fmt[5] = { 0x400, 0x400, 0x200, 0x400, 0x400 };  // TMEM bytes: CI keeps half for the TLUT
    uint16_t L = a_fmt[imageFmt];
    uint32_t M = a_size[imageSiz];
    uint16_t N = (uint16_t)(((frameW * scaleW) >> 7) + (K << 5)), O = N >= Fw ? Fw : N;
    uint16_t P = (uint16_t)((((O + (M >> 16)) * (M & 0xFFFF)) >> 16) + 1);
    uint32_t w550 = (uint32_t)K << 24 | (uint32_t)J_2 << 16 | P;

    uint16_t Q = (uint16_t)(L / (P * 2) + K * 0xFFFF);
    int32_t R = (int32_t)((0x100000 * (uint32_t)Q) / scaleH);
    if (R <= 0) os_die("rsp gfx: S2DEX2 G_BG_1CYC strip step", (uint32_t)R);
    int32_t S = (int32_t)((((int64_t)I * 0x4000000 / scaleH) >> 16) & 0xFFFFFC00);
    int16_t T = (int16_t)(((int64_t)(0xFFFFFFFFu / (uint32_t)R) * (int64_t)S) >> 32);
    int32_t U = R * (T + 1);
    int16_t V = (int16_t)(T + (U <= S));
    int32_t W = R * V, Z = R - S + W;

    uint32_t A1 = (uint32_t)S - ((uint32_t)W & 0xFFFFFC00u), B1 = (A1 * 0x0040) >> 16, C1 = B1 * scaleH;
    uint16_t D1 = (uint16_t)(((C1 * 0x0040) >> 16) & 0xFFFF), E1 = (uint16_t)(Q - D1), F1 = (uint16_t)C1;
    uint16_t F1_1 = (uint16_t)((((uint32_t)F1 << 11) >> 16) & 0x1F);

    int32_t yo11 = (int32_t)((uint32_t)imageYorig << 11) >> 16;
    int16_t A2 = (int16_t)(((uint32_t)Q * (uint32_t)(int32_t)V + D1 + (uint32_t)yo11) & 0xFFFF);
    uint16_t B2 = (uint16_t)(((uint32_t)imageH << 14) >> 16);
    int16_t A2_1 = (int16_t)(A2 >= 0 ? A2 : A2 + B2);
    if (A2 - B2 >= 0) A2_1 = (int16_t)(A2_1 - B2);
    int16_t C2 = (int16_t)((((uint32_t)(int32_t)Gw * (M & 0xFFFF)) >> 16) << 3);
    int16_t D2 = (int16_t)((((uint32_t)Fw * (M & 0xFFFF)) >> 16) << 3);
    uint32_t E2_1 = (uint32_t)(A2_1 * D2 + C2) + imagePtr;
    int32_t F2 = E1 * D2, G2 = Q * D2;
    int16_t H2 = (int16_t)((uint32_t)(int32_t)Gw & (M >> 16));
    // The ucode flips by starting s at -(H2 + J) on a tile mirrored in s; on the clamped strip tile that is s stepping
    // down from H2 + J, a 1/32 texel short so whole texels mirror to the same index
    uint16_t s_start = flips ? (uint16_t)(H2 + J - 1) : (uint16_t)H2, dsdx = flips ? (uint16_t)-scaleW : scaleW;
    uint32_t I2 = 0xFD100000u | (uint32_t)((D2 >> 1) - 1);
    uint32_t J2 = 0xF5100000u | (uint32_t)P << 9;
    uint32_t J2_1 = (J2 & 0xFF00FFFFu) | (uint32_t)((imageFmt << 5) | (imageSiz << 3)) << 16;
    uint32_t K2 = (uint32_t)imagePal << 20 | 0x00080200u;  // the ucode's 0x0007C1F0, masks 15, here clamped (strip_tile)
    uint16_t L2 = (uint16_t)(imageH >> 2);
    b->w550 = w550, b->w564 = (uint32_t)((int32_t)C2 << 16 | D2), b->w570 = (uint32_t)E1 << 16 | Q;
    b->w57c = (uint32_t)((int32_t)L2 << 16 | A2_1);
    b->imagePtr = imagePtr, b->E2_1 = E2_1, b->I2 = I2, b->J2 = J2, b->Z = (uint32_t)Z, b->R = (uint32_t)R;
    b->F2 = F2, b->G2 = G2, b->P = P, b->F1_1 = F1_1, b->s_start = s_start, b->dsdx = dsdx, b->scaleH = scaleH;
    b->siz = imageSiz;
    gfx_rdp(J2, 0x07000000u);
    gfx_rdp(J2_1, K2);
    gfx_rdp((uint32_t)G_SETTILESIZE << 24, 0);
}

// SETTILE, SETTIMG and LOADTILE: rows lines of line words from src, into TMEM at tmem words
static void bg1_load_rows(const struct bg1 *b, uint32_t tmem, uint32_t src, uint16_t line, int16_t rows) {
    gfx_rdp(b->J2 | tmem, 0x07000000u);
    gfx_rdp(b->I2, src);
    gfx_rdp((uint32_t)G_LOADTILE << 24, (((uint32_t)line + 0x6FF) << 16) | (((uint32_t)(int32_t)rows << 2) - 1));
}

// A strip that runs past the image's last row: the rows from its first row up, then the line the wrap splits
static void bg1_wrap(const struct bg1 *b, int16_t AA, int16_t MM, int16_t NN, uint32_t E2_1) {
    uint16_t Pv = (uint16_t)(b->w550 & 0xFFFF);
    int16_t RR = (int16_t)(MM - AA);
    uint32_t AAA = (uint32_t)(int32_t)AA, SS;
    if (RR > 0) {
        SS = b->imagePtr + (b->w564 >> 16);
        if (AAA & 1) AAA--, RR++, SS -= b->w564 & 0xFFFF;
        bg1_load_rows(b, AAA * Pv, SS, Pv, RR);
    }
    if (!((b->w550 >> 16) & 0xFF)) return;
    SS = b->imagePtr;
    int16_t BBB = NN;
    RR = (int16_t)(NN & 1);
    if (RR) BBB--, SS -= b->w564 & 0xFFFF;
    uint32_t CCC = E2_1 + (uint32_t)(int32_t)BBB * (b->w564 & 0xFFFF);
    RR++;
    uint16_t DDD = (uint16_t)((((b->w564 & 0xFFFF) - (b->w564 >> 16)) * 0x2000) >> 16);
    uint32_t ZZZ = (uint32_t)(int32_t)BBB * Pv;
    bg1_load_rows(b, ZZZ + DDD, SS, (uint16_t)(Pv - DDD), RR);
    bg1_load_rows(b, ZZZ, CCC, DDD, RR);
}

// The next strip's first image row: on down the image, or wrapped to its top
static uint32_t bg1_next_src(const struct bg1 *b, int16_t *AA, uint32_t E2_1, uint32_t GG) {
    if (*AA > 0) return E2_1 + GG;
    uint32_t src = b->imagePtr + (b->w564 >> 16) + (b->w564 & 0xFFFF) * (uint32_t)(-*AA);
    *AA = (int16_t)(*AA + (int32_t)(b->w57c >> 16));
    return src;
}

// Part 2: per strip, load the rows it needs (wrapping to the image's first row) and draw it
static void bg1_strips(const struct bg1 *b) {
    int16_t AA = (int16_t)((b->w57c >> 16) - (b->w57c & 0xFFFF));
    int32_t CC = (int32_t)(b->w570 >> 16), FF = (int32_t)(b->w54c & 0xFFFF);
    uint32_t DD = b->Z, E2_1 = b->E2_1, GG = (uint32_t)b->F2, HH = (b->w548 >> 16) << 12, II = (b->w548 & 0xFFFF) << 12;
    uint16_t JJ = (uint16_t)(b->w54c >> 16), F1_1 = b->F1_1, Pv = (uint16_t)(b->w550 & 0xFFFF);
    for (;;) {
        int32_t KK = (int32_t)(DD >> 10);
        if (KK <= 0) {  // a strip of no output rows: its source rows are skipped
            AA = (int16_t)(AA - CC);
            E2_1 = bg1_next_src(b, &AA, E2_1, GG);
        } else {
            FF -= KK;
            DD &= 0x03FF;
            if (FF < 0) {
                CC += ((b->scaleH * FF) >> 10) + 1;
                KK += FF;
                if (CC - (int32_t)(b->w570 & 0xFFFF) > 0) CC = (int32_t)(b->w570 & 0xFFFF);
            }
            int16_t LL = (int16_t)(JJ + KK), MM = (int16_t)(CC + (int32_t)(b->w550 >> 24));
            int16_t NN = (int16_t)(AA - (int32_t)((b->w550 >> 16) & 0xFF));
            uint32_t rows = (uint32_t)(MM > 0 ? MM : 1);
            if (NN - MM < 0) {
                bg1_wrap(b, AA, MM, NN, E2_1);
                AA = (int16_t)(AA - CC);
                if (NN <= 0) gfx_rdp(b->J2, 0x07000000u);
                else bg1_load_rows(b, 0, E2_1, Pv, NN);
                E2_1 = bg1_next_src(b, &AA, E2_1, GG);
            } else {
                gfx_rdp(b->I2, E2_1);
                gfx_rdp((uint32_t)G_LOADTILE << 24, (((uint32_t)Pv + 0x6FF) << 16) | (((uint32_t)(int32_t)MM << 2) - 1));
                AA = (int16_t)(AA - CC);
                E2_1 += GG;
            }
            strip_tile(b->P, b->siz, rows);
            gfx_rdp((uint32_t)G_TEXRECT << 24 | (uint32_t)(int32_t)LL << 2 | II, (uint32_t)JJ << 2 | HH);
            gfx_rdp((uint32_t)b->s_start << 16 | F1_1, (uint32_t)b->dsdx << 16 | b->scaleH);
            if (FF <= 0) return;
            JJ = (uint16_t)LL;
        }
        DD += b->R;
        CC = (int32_t)(b->w570 & 0xFFFF);
        GG = (uint32_t)b->G2;
        F1_1 = 0;
    }
}

// G_BG_1CYC (S2DEX2 overlay 3) as GLideN64's BgRect1CycStripped decodes it: TMEM-sized strips of the image, each one
// SETTILE + SETTIMG + LOADTILE (two when it wraps past the image's last row) and a scaled TEXRECT
void s2dex_bg_1cyc(uint32_t bg, uint32_t rendermode) {
    uint8_t imageFmt = gfx_rd8(bg + 0x16), imageSiz = gfx_rd8(bg + 0x17);
    uint16_t scaleW = rd16(bg + 0x1C), scaleH = rd16(bg + 0x1E);
    if (!scaleW || !scaleH || imageSiz > 3 || imageFmt > 4) os_die("rsp gfx: S2DEX2 G_BG_1CYC scale, size or format", bg);
    struct bg1 b;
    int16_t edge[4];
    if (!bg1_clip(bg, rd16(bg + 0x1A) & G_BG_FLAG_FLIPS, scaleW, scaleH, edge, &b)) return;
    bg1_setup(bg, rendermode, edge, &b);
    bg1_strips(&b);
}
