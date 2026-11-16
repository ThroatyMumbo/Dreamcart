#ifndef GFX_CC_H
#define GFX_CC_H

#include "htypes.h"

struct cc_plan {
    uint8_t textured, decal, rgb_alpha, alpha_tex, alpha_const;
    uint8_t env, ignore_tex_alpha, offset, fog, blend_fog_alpha;
    uint8_t list, src, dst, zmode, approx;
    uint8_t alpha_cvg;  // the RDP draws nothing, depth included, where the texel alpha leaves no coverage
    uint8_t alpha_test;  // unblended, alpha compare against blend alpha, and the combined alpha is the texel's
    uint8_t tex_invert;  // rgb channels (bit 0 red) taken as 255 - texel: k·T + c = -k·(1 - T) + (c + k), for k < 0
    uint8_t affine;     // every output is k0 + k1 * shade_channel + k2 * shade_alpha: k below
    uint8_t no_offset;  // the offset color's rgb is 0 for every shade
    float out[7][3];    // the vertex's outputs from k: base r, g, b, a, then offset r, g, b, in 0..255 units
};

// The PVR mapping of the current combiner, othermodes and blender; rect for texture/fill rectangles
void gfx_cc_plan(struct cc_plan *p, int texture_on, int rect);
void gfx_cc_texel_tiles(int *rgb_off, int *alpha_off);  // tile + these feed the texture's color and its alpha
// One vertex's PVR base color and offset color (offset alpha carries the fog factor)
void gfx_cc_vertex(const struct cc_plan *p, const uint8_t shade[4], uint32_t *argb, uint32_t *oargb);

#endif
