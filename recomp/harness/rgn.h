#ifndef RGN_H
#define RGN_H

#include <stdint.h>

// Run-time code (docs/n64_any_cart.md, "Run-time code"): regions of RDRAM code the game wrote and passed to
// osInvalICache, scanned whole, translated a chunk at a time on demand and cached on the reader's SD; else one function
void rgn_note(uint32_t vaddr, uint32_t bytes);  // osInvalICache: a new region, and live code over it is suspect
uint32_t rgn_code(uint32_t vram);              // a lookup miss: vram's code, translated, or 0 outside every region
uint32_t rgn_guest_fn(uint32_t h);             // the guest function holding host address h, else 0
uint32_t rgn_any_guest_fn(uint32_t h);         // the same over an overlay's code (ovl.c) too
void rgn_report(void);                         // the working set so far, one line per region
void rgn_init(void);                           // after ovl's init: takes t_lookup's misses and osInvalICache

// The reader's SD for the regions' scans, by two key words: get returns the blob's length (0: none) in a cs_alloc'd
// buffer; NULLs scan every region afresh
void rgn_xcache(uint32_t (*get)(void *ctx, uint32_t a, uint32_t b, uint8_t **blob),
                int (*put)(void *ctx, uint32_t a, uint32_t b, const uint8_t *blob, uint32_t len), void *ctx);

#endif
