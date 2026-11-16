#ifndef GFX_PVR_H
#define GFX_PVR_H

#include <stdint.h>
#include "gfx_sink.h"

struct gfx_pvr_stats { uint32_t headers, tris, uploads, dropped, missing_tex, vram_full, readback_rows, kept_frames; uint64_t wait_us; uint32_t entries[3]; };  // entries: OP, PT, TR

void gfx_pvr_init(void);  // 640x480 RGB565, PVR lists OP/PT/TR, no translucent autosort
int gfx_pvr_idle(void);   // from idle waits: moves the last frame back into RDRAM a row at a time; 0 once it's all there
uint32_t gfx_pvr_front(void);  // the shown 640x480 RGB565 frame's linear address in 32-bit VRAM
// A 16-bit framebuffer the CPU drew, at RDRAM phys, as one textured quad; 0 while a graphics frame is open
int gfx_pvr_show_rdram(uint32_t phys, int w, int h);
int gfx_pvr_drew(uint32_t phys);  // phys is inside a recent graphics frame's color image: the PVR holds its pixels
extern const struct gfx_sink gfx_sink_pvr;
extern struct gfx_pvr_stats gfx_pvr_stats;

#endif
