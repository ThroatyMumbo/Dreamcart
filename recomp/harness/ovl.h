#ifndef OVL_H
#define OVL_H

#include <stddef.h>
#include <stdint.h>

#include "boot.h"
#include "xlate.h"

// Overlays translated on load into an arena sized for the most resident at once (docs/n64_any_cart.md); ovl_reserve
// sizes it from the scan once a first boot has freed the scan, ovl_init takes c over after tload
void ovl_reserve(const CsResult *r);
void ovl_init(CbCache *c, uint32_t rom_size, void (*sync)(void *p, size_t n));
// Every other cart: no ROM overlays, only the code the game writes to RDRAM (rgn.c); takes c over
void ovl_runtime_init(CbCache *c, uint32_t rom_size, void (*sync)(void *p, size_t n));
// The reader's translation cache (CartSrc's xc_get/xc_put) for ROM overlays; NULLs translate every one
void ovl_xcache(int (*get)(void *ctx, uint32_t meta_crc, uint32_t meta_len, XlOut *o),
                int (*put)(void *ctx, uint32_t meta_crc, uint32_t meta_len, const XlOut *o), void *ctx);
// Residency, from the ovtab tload set: a PI DMA over an overlay's range loads it when it keeps the overlay's
// ROM-to-vram delta, else evicts it
void ov_dma(uint32_t rom_off, uint32_t dram, uint32_t len);  // rcp.c, on every PI DMA
uint32_t ovl_key(uint32_t vram);  // the key of the overlay function at vram: the only overlay there, or the one loaded
uint32_t ovl_need_key(uint32_t key);  // key's code, translated now if its overlay is loaded but untranslated; else 0
uint32_t ovl_arena_bytes(void);
uint32_t ovl_work_bytes(void);  // the heap one translation needs on top of the arena
uint32_t ovl_guest_fn(uint32_t h);  // the guest function holding host address h in an overlay's code, else 0

// For rgn.c: the fixed code's scan (and the cart's hooks and raw flags), and one translation linked in place
typedef uint8_t *(*OvlPlace)(uint32_t bytes, uint32_t who, uint32_t *off);
CbCache *ovl_ctx(void);
uint32_t ovl_rom_size(void);
int ovl_read_page(uint32_t pg, uint32_t *w);  // cb_read_page through rcp_cart_read
const uint32_t *ovl_page(void *ctx, uint32_t pg);  // a cs_page_fn over the ROM, one page kept
uint8_t *ovl_place_heap(uint32_t n, uint32_t who, uint32_t *off);
uint8_t *ovl_place_ram(uint32_t n, uint32_t who, uint32_t *off);  // in the arena when there is one, else the heap
void ovl_arena_drop(uint32_t who);  // frees who's arena block
uint8_t *ovl_link_meta(const uint8_t *meta, uint32_t mlen, uint32_t ins, int cached, uint32_t who, uint32_t vram,
                       OvlPlace place, uint32_t (*lookup)(uint32_t), uint32_t *off, uint32_t *len, uint32_t **keys,
                       uint32_t *nkey, XlStats *xs);

#endif
