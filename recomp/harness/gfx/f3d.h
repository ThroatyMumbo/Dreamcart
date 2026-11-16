#ifndef F3D_H
#define F3D_H

// Fast3D (F3D_OLD, "RSP SW Version: 2.0D, 04-01-96") opcodes, from the decomp's include/PR/gbi.h and rsp/fast3d.s;
// F3DEX 0.95's additions are from its disassembly (docs/n64_gfx_hle.md §7)
#define F3D_SPNOOP 0x00
#define F3D_MTX 0x01
#define F3D_MOVEMEM 0x03
#define F3D_VTX 0x04
#define F3D_DL 0x06
#define F3D_RDPHALF_CONT 0xB2
#define F3D_RDPHALF_2 0xB3
#define F3D_RDPHALF_1 0xB4
#define F3D_LINE3D 0xB5
#define F3D_CLEARGEOMETRYMODE 0xB6
#define F3D_SETGEOMETRYMODE 0xB7
#define F3D_ENDDL 0xB8
#define F3D_SETOTHERMODE_L 0xB9
#define F3D_SETOTHERMODE_H 0xBA
#define F3D_TEXTURE 0xBB
#define F3D_MOVEWORD 0xBC
#define F3D_POPMTX 0xBD
#define F3D_CULLDL 0xBE
#define F3D_TRI1 0xBF

// F3DEX/F3DLX 0.95 (F3DEX_GBI + F3D_OLD): Fast3D's numbers plus these; 0xAF and 0xB0 are SP_NOOP in its jump table
#define F3DEX_LOAD_UCODE 0xAF
#define F3DEX_BRANCH_Z 0xB0
#define F3DEX_TRI2 0xB1
#define F3DEX_QUAD 0xB5

// F3DEX2 (GBI2), from gbi.h's F3DEX_GBI_2 and Mr-Wiseguy/f3dex2's f3dex2.s, which matches Kirby 64's 2.04H fifo
#define X2_VTX 0x01
#define X2_MODIFYVTX 0x02
#define X2_CULLDL 0x03
#define X2_BRANCH_Z 0x04
#define X2_TRI1 0x05
#define X2_TRI2 0x06
#define X2_QUAD 0x07
#define X2_LINE3D 0x08
#define X2_SPECIAL_3 0xD3
#define X2_SPECIAL_2 0xD4
#define X2_SPECIAL_1 0xD5  // 2.04H: recompute MP now
#define X2_DMA_IO 0xD6
#define X2_TEXTURE 0xD7
#define X2_POPMTX 0xD8
#define X2_GEOMETRYMODE 0xD9
#define X2_MTX 0xDA
#define X2_MOVEWORD 0xDB
#define X2_MOVEMEM 0xDC
#define X2_LOAD_UCODE 0xDD
#define X2_DL 0xDE
#define X2_ENDDL 0xDF
#define X2_SPNOOP 0xE0
#define X2_RDPHALF_1 0xE1
#define X2_SETOTHERMODE_L 0xE2
#define X2_SETOTHERMODE_H 0xE3
#define X2_RDPHALF_2 0xF1

#define X2_MTX_NOPUSH 0x01  // G_MTX's flags arrive XORed with G_MTX_PUSH
#define X2_MTX_LOAD 0x02
#define X2_MTX_PROJECTION 0x04
#define X2_MV_POINT 12

#define X2G_CULL_FRONT 0x00000200
#define X2G_CULL_BACK 0x00000400
#define X2G_SHADING_SMOOTH 0x00200000

// F3DEX2 2.04H DMEM; G_LOAD_UCODE reloads only from X2_FXBASE up
#define X2_MV 0x000
#define X2_P 0x040
#define X2_MVP 0x080
#define X2_MTX_END 0x0C0
#define X2_OTHERMODE 0x0C8
#define X2_TEXRECT 0x0D0
#define X2_HALF1 0x0D8
#define X2_VIEWPORT 0x0E0
#define X2_MTX_STACK 0x0F4
#define X2_SEGMENTS 0x0F8
#define X2_FXBASE 0x180
#define X2_CLIP_RATIO 0x186
#define X2_MVP_VALID 0x1D9
#define X2_LIGHTS_VALID 0x1DC
#define X2_NUMLIGHTS 0x1DD  // lights times 0x18
#define X2_FOG 0x1E0
#define X2_TEXTURE_W0 0x1E4
#define X2_GEOM 0x1EC
#define X2_LOOKAT 0x1F0  // lookat X, Y, then the lights (X2_LIGHTS) and ambient after the last
#define X2_LIGHTS 0x220
#define X2_LIGHT_SIZE 0x18  // RGB at +0, dir at +8, the dir in model space at +0x10 and +0x14
#define X2_MOVEMEM_TAB 0x2F0
#define X2_MOVEWORD_TAB 0x2FE
#define X2_VTXBUF 0x420
#define X2_VTX_SIZE 0x28

#define G_MTX_PROJECTION 0x01
#define G_MTX_LOAD 0x02
#define G_MTX_PUSH 0x04

#define G_MV_VIEWPORT 0x80
#define G_MV_LOOKATY 0x82
#define G_MV_LOOKATX 0x84
#define G_MV_L0 0x86
#define G_MV_L7 0x94
#define G_MV_TXTATT 0x96
#define G_MV_MATRIX_1 0x9E
#define G_MV_MATRIX_2 0x98
#define G_MV_MATRIX_3 0x9A
#define G_MV_MATRIX_4 0x9C

#define G_MW_MATRIX 0x00
#define G_MW_NUMLIGHT 0x02
#define G_MW_CLIP 0x04
#define G_MW_SEGMENT 0x06
#define G_MW_FOG 0x08
#define G_MW_LIGHTCOL 0x0A
#define G_MW_POINTS 0x0C
#define G_MW_PERSPNORM 0x0E
#define G_MWO_POINT_RGBA 0x10  // gSPModifyVertex: offsets into a 40-byte points-buffer entry
#define G_MWO_POINT_ST 0x14
#define G_MWO_POINT_XYSCREEN 0x18
#define G_MWO_POINT_ZSCREEN 0x1C  // F3DEX2

#define G_ZBUFFER 0x00000001
#define G_TEXTURE_ENABLE 0x00000002  // Fast3D's G_TEXTURE on bit; the HLE keeps it for every ucode
#define G_SHADE 0x00000004
#define G_SHADING_SMOOTH 0x00000200
#define G_CULL_FRONT 0x00001000
#define G_CULL_BACK 0x00002000
#define G_FOG 0x00010000
#define G_LIGHTING 0x00020000
#define G_TEXTURE_GEN 0x00040000
#define G_TEXTURE_GEN_LINEAR 0x00080000
#define G_CLIPPING 0x00800000  // F3DLX only: clear means no trivial reject and no clipping

// RDP commands the RSP forwards (0xC0-0xFF)
#define G_NOOP 0xC0
#define G_TEXRECT 0xE4
#define G_TEXRECTFLIP 0xE5
#define G_RDPLOADSYNC 0xE6
#define G_RDPPIPESYNC 0xE7
#define G_RDPTILESYNC 0xE8
#define G_RDPFULLSYNC 0xE9
#define G_SETKEYGB 0xEA
#define G_SETKEYR 0xEB
#define G_SETCONVERT 0xEC
#define G_SETSCISSOR 0xED
#define G_SETPRIMDEPTH 0xEE
#define G_RDPSETOTHERMODE 0xEF
#define G_LOADTLUT 0xF0
#define G_SETTILESIZE 0xF2
#define G_LOADBLOCK 0xF3
#define G_LOADTILE 0xF4
#define G_SETTILE 0xF5
#define G_FILLRECT 0xF6
#define G_SETFILLCOLOR 0xF7
#define G_SETFOGCOLOR 0xF8
#define G_SETBLENDCOLOR 0xF9
#define G_SETPRIMCOLOR 0xFA
#define G_SETENVCOLOR 0xFB
#define G_SETCOMBINE 0xFC
#define G_SETTIMG 0xFD
#define G_SETZIMG 0xFE
#define G_SETCIMG 0xFF

// OSTask in DMEM, as osSpTaskLoad leaves it
#define TASK_TYPE 0xFC0
#define TASK_FLAGS 0xFC4
#define TASK_UCODE_BOOT 0xFC8
#define TASK_UCODE 0xFD0
#define TASK_UCODE_DATA 0xFD8
#define TASK_DRAM_STACK 0xFE0
#define TASK_DRAM_STACK_SIZE 0xFE4
#define TASK_OUTPUT_BUFF 0xFE8
#define TASK_DATA_PTR 0xFF0
#define TASK_DATA_SIZE 0xFF4

#endif
