#ifndef GFX_TEX_H
#define GFX_TEX_H

#include "htypes.h"

struct tex_bind {
    uint16_t id, w, h;
    uint8_t clamp_u, clamp_v, flip_u, flip_v, filter, shifts, shiftt;
    uint8_t step_s, step_t, phase_s, phase_t;  // the decimation applied: texel phase + i * step is texel i
    uint16_t uls, ult;
};

// Every step-th texel from phase on (relative to the tile), for a rect that steps whole texels; 1, 0 for all of them
struct tex_dec { uint8_t step_s, step_t, phase_s, phase_t; };

// What the combiner bakes into the texels: alpha_ref >= 0 makes alpha 0xFF where it is >= alpha_ref, else 0 (the RDP's
// alpha compare); invert stores rgb channels (bit 0 red) as 255 - c
struct tex_fx { int16_t alpha_ref; uint8_t invert; };

// Decodes (or finds) the texture for a tile and returns its sink id and sampling state; alpha_tile >= 0 supplies alpha
int gfx_tex_bind(int tile, int alpha_tile, struct tex_fx fx, const struct tex_dec *want, struct tex_bind *b);
void gfx_tex_reset(void);
void gfx_tex_frame(void);  // once per frame: slots bound in the last two frames are not evicted
extern uint32_t gfx_tex_uploads, gfx_tex_lookups, gfx_tex_forced;

#endif
