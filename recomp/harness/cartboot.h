#ifndef CARTBOOT_H
#define CARTBOOT_H

#include <stddef.h>
#include <stdint.h>

#include "boot.h"
#include "xlate.h"

// Everything a ROM needs before it boots, done from the ROM itself: scan, libultra signatures, the power-on RDRAM
// image, the translation, and the link in RAM. Fills cart_info; afterwards t_lookup_init() finds the code.
typedef struct {
    uint32_t size;                                                  // ROM bytes
    int (*read)(void *ctx, uint32_t off, void *dst, uint32_t len);  // z64 byte order; 0 on failure
    // Optional, the adapter's whole-ROM pass: the jr ra starts and the pointer-like words in [ptr_lo, ptr_hi),
    // sorted and unique, in cs_alloc'd arrays. NULL, or 0 returned: the ROM is streamed here instead.
    int (*tables)(void *ctx, uint32_t **starts, uint32_t *nstarts, uint32_t **ptrs, uint32_t *nptrs, uint32_t *ptr_lo,
                  uint32_t *ptr_hi);
    // Optional, the per-cart cache: get returns the blob's length (0: none) in a cs_alloc'd buffer
    uint32_t (*cache_get)(void *ctx, uint8_t **blob);
    int (*cache_put)(void *ctx, const uint8_t *blob, uint32_t len);
    void *ctx;
    // Optional, xlate's output for a meta, kept by the reader (xlpack.h): get fills o and returns 1, else leaves it
    // zeroed; put is told each translation before it is linked. Overlays use them too.
    int (*xc_get)(void *ctx, uint32_t meta_crc, uint32_t meta_len, XlOut *o);
    int (*xc_put)(void *ctx, uint32_t meta_crc, uint32_t meta_len, const XlOut *o);
    // Optional, any blob the reader keeps by two key words (rgn.c's region scans): get returns its length (0: none)
    uint32_t (*xb_get)(void *ctx, uint32_t a, uint32_t b, uint8_t **blob);
    int (*xb_put)(void *ctx, uint32_t a, uint32_t b, const uint8_t *blob, uint32_t len);
} CartSrc;

typedef struct {
    uint64_t us_stream, us_scan, us_sigs, us_boot, us_meta, us_xlate, us_load, us_cache;
    uint32_t nfunc, nhooked, mips, text, nrel, meta_len, meta_crc, boot_crc, text_crc, rel_crc, stubs;
    uint32_t cached, cache_len, cache_stored, tables;  // tables: 1 when the adapter did the whole-ROM pass
    uint32_t xc_hit, xc_stored;                         // the translation came from, or went to, the reader's SD
    uint64_t us_xc_put;
} CartBootStats;

enum { CB_STAGE_CACHE, CB_STAGE_TABLES, CB_STAGE_SCAN, CB_STAGE_SIGS, CB_STAGE_META, CB_STAGE_STORE, CB_STAGE_IMAGE,
       CB_STAGE_XC_GET, CB_STAGE_XLATE, CB_STAGE_XC_PUT, CB_STAGE_LINK, CB_STAGE_N };
// The platform's glue (dc/play_cartboot.c and dc/cartboot_kos.c) defines these
void cartboot_stage(int stage);  // told as each stage starts, from cartboot's thread
void cartboot_aborting(void);    // the KOS glue's last call before an out-of-memory or die abort
uint32_t cartboot_heap_used(void);
// KOS glue: cs_alloc and xl_alloc overflow into [base, base + len) until close, which aborts if anything is left there
void cartboot_spill_open(void *base, size_t len);
void cartboot_spill_close(void);

// rdram: BOOT_RDRAM bytes of host-order words (each the big-endian word at that RDRAM offset). sync writes back the
// D-cache and invalidates the I-cache over freshly written code.
const char *cartboot(const CartSrc *src, uint32_t *rdram, XlOut *xo, CartBootStats *st, void (*sync)(void *p, size_t n),
                     uint64_t (*now_us)(void));

// ROM page pg as 1024 host-order words, zeros past size; 0 when the read fails
int cb_read_page(int (*read)(void *ctx, uint32_t off, void *dst, uint32_t len), void *ctx, uint32_t size, uint32_t pg,
                 uint32_t *w);

// Per scan function, whether it takes every register (xlate's raw pass over each segment's XLM3), size its page space
uint8_t *cb_raw_bits(const CsResult *r, const CbHooks *hk, cs_page_fn page, void *ctx, uint32_t size);

#endif
