#ifndef RSP_GFX_H
#define RSP_GFX_H

#include "htypes.h"

struct gfx_stats {
    uint32_t cmds, dls, vtx, tris, rdp, hash;
    int fullsync;
};

// Runs the graphics task whose OSTask is at DMEM 0xFC0 (word-swapped SP memory). It never writes RDRAM.
void rsp_gfx_task(const uint8_t *dmem, struct gfx_stats *st);
extern void (*rsp_gfx_poll)(void);  // every 32 commands; it may switch fibers and come back later
void gfx_tmem_flush(void);  // before a task parks: the CPU may then write RDRAM under a load not yet in TMEM

struct gfx_ucode;
const struct gfx_ucode *gfx_ucode_find(uint32_t data);  // by the version string in its data; dies on any other
void rsp_gbi2_task(const uint8_t *dmem, struct gfx_stats *st);  // F3DEX2, once rsp_gfx_task has loaded its DMEM data

#endif
