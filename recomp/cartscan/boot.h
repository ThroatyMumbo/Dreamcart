#ifndef CARTSCAN_BOOT_H
#define CARTSCAN_BOOT_H

#include "scan.h"
#include "sigs.h"

// Power-on RDRAM (IPL3's 1 MB copy, entry.s's bss clear, IPL3's globals at 0x300)
typedef struct { uint32_t entry, bss, bss_size, main, sp, header[16], sp_fill, imem0; } BootInfo;

#ifndef RDRAM_BYTES
#define RDRAM_BYTES 0x400000u  // harness/htypes.h's default
#endif
#define BOOT_RDRAM RDRAM_BYTES

// img holds BOOT_RDRAM bytes as host-order words, each the big-endian word at that RDRAM offset
const char *bi_build(uint32_t *img, cs_page_fn page, void *ctx, uint32_t rom_size, BootInfo *bi);

// The XLM2 blob xlate's load_meta reads
typedef struct { uint32_t n, *addr; uint16_t *pol; } CbHooks;  // hooked functions: address, index into pol_*
void hooks_from_hm(const HmResult *hm, CbHooks *hk);
uint8_t *meta_write(const CsResult *scan, const CbHooks *hk, cs_page_fn page, void *ctx, uint32_t rom_size,
                    uint32_t *len);
// XLM3 for one-at-a-time overlays: seg < 0 is every fixed segment, else that overlay (outside overlay key space, a run-time
// chunk's functions). Adds the overlays and functions left out, with raw[i] (NULL: all 0) per scan function.
uint8_t *meta_write_seg(const CsResult *scan, const CbHooks *hk, const uint8_t *raw, int seg, cs_page_fn page,
                        void *ctx, uint32_t rom_size, uint32_t *len);
int meta_seg_is_ovl(const CsResult *scan, uint32_t seg);
int meta_seg_holds(const CsResult *scan, int seg, uint32_t v);  // v is in seg (seg < 0: in no overlay)

// The per-cart cache of what the scan and the signatures decided (kept on the adapter's flash)
typedef struct {
    CsResult r;      // segments, functions (start, end) and jump tables
    CbHooks hk;
    uint32_t os[4];  // thread tail, event table, RCP mask table, osDestroyThread
    uint8_t *raw;    // per function, whether it takes every register; NULL when the blob has none
} CbCache;
uint32_t cache_version(void);
uint8_t *cache_build(const CsResult *scan, const HmResult *hm, const uint8_t *raw, uint32_t rom_size,
                     const uint32_t header[16], uint32_t *len);
int cache_parse(const uint8_t *blob, uint32_t len, uint32_t rom_size, const uint32_t header[16], CbCache *v);

#endif
