#ifndef RSP_S2DEX_H
#define RSP_S2DEX_H

#include "htypes.h"

// S2DEX2 2.04 DMEM above spFxBase (its data; below it the layout is F3DEX2's)
#define S2_MOVEWORD_TAB 0x386  // + G_MW_* index: 2 0x1E0, 4 0x1F0, 6 the segments, 8 the status words
#define S2_MOVEMEM_TAB 0x388   // + G_OBJ_MOVEMEM index: 0 the object matrix, 2 its sub-matrix
#define S2_STATUS 0x2B0        // four status words, for G_OBJ_LOADTXTR and G_SELECT_DL
#define S2_RENDERMODE 0x268    // G_OBJ_RENDERMODE's low byte (overlay 1 at 0x10E8: sb w1)
#define G_BG_FLAG_FLIPS 0x01
#define G_BGLT_LOADBLOCK 0x0033
#define G_BGLT_LOADTILE 0xFFF4

#define S2_OBJ_RECTANGLE 0x01
#define S2_OBJ_SPRITE 0x02
#define S2_SELECT_DL 0x04
#define S2_OBJ_LOADTXTR 0x05
#define S2_OBJ_LDTX_SPRITE 0x06
#define S2_OBJ_LDTX_RECT 0x07
#define S2_OBJ_LDTX_RECT_R 0x08
#define S2_BG_1CYC 0x09
#define S2_BG_COPY 0x0A
#define S2_OBJ_RENDERMODE 0x0B
#define S2_OBJ_RECTANGLE_R 0xDA
#define S2_OBJ_MOVEMEM 0xDC

void s2dex_obj_loadtxtr(uint32_t phys);
void s2dex_bg_1cyc(uint32_t phys, uint32_t rendermode);
void s2dex_bg_copy(uint32_t phys);

#endif
