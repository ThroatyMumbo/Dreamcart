#ifndef GFX_SINK_H
#define GFX_SINK_H

#include "htypes.h"

// The graphics HLE targets the PVR: the sink takes PVR polygon state, and these are its encodings (lists, TXRENV,
// blend factors, depth compare, texture formats); gfx_sh4.h holds the SH-4 matrix code
enum { GL_OP, GL_PT, GL_TR };
enum { TXR_REPLACE, TXR_MODULATE, TXR_DECAL, TXR_MODULATEALPHA };
enum { BL_ZERO, BL_ONE, BL_DESTCOLOR, BL_INVDESTCOLOR, BL_SRCALPHA, BL_INVSRCALPHA, BL_DESTALPHA, BL_INVDESTALPHA };
enum { DC_NEVER, DC_LESS, DC_EQUAL, DC_LEQUAL, DC_GREATER, DC_NOTEQUAL, DC_GEQUAL, DC_ALWAYS };
enum { TF_ARGB1555, TF_RGB565, TF_ARGB4444 };

struct gfx_poly {
    uint8_t list, txr_env, src, dst;
    uint8_t depth_cmp, depth_write, offset, fog;
    uint8_t textured, filter, clamp_u, clamp_v;
    uint8_t flip_u, flip_v, ignore_tex_alpha;
    uint8_t underlay;  // TR: the sink makes depth_cmp ALWAYS unless a later OP or PT poly writes depth
    uint16_t tex_id, tex_w, tex_h, pad2;
    uint32_t fog_argb;
};

struct gfx_pv {
    float x, y, z, u, v;
    uint32_t argb, oargb;
};

#define GFX_COVER_COLS 32
#define GFX_COVER_ROWS 24
struct gfx_cover { uint32_t row[GFX_COVER_ROWS]; };  // a 32x24 grid over the frame; bit c of row r: cell (c, r) drawn in

struct gfx_sink {
    void (*frame_begin)(uint32_t cimg, int siz, int w, int h);  // siz as G_SETCIMG: 2 for 16-bit
    void (*poly)(const struct gfx_poly *p);
    void (*tri)(const struct gfx_pv *a, const struct gfx_pv *b, const struct gfx_pv *c);
    void (*tex_upload)(uint16_t id, int w, int h, int fmt, const uint16_t *texels);
    // the cells it drew in (the color image keeps the rest), and how many rows of the color image it can have written
    void (*frame_end)(const struct gfx_cover *drawn, int rows);
};

extern const struct gfx_sink *gfx_sink;  // dc/gfx_pvr.c's, set before the first task

#endif
