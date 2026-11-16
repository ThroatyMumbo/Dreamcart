#ifndef CARTSCAN_SCAN_H
#define CARTSCAN_SCAN_H

#include <stddef.h>
#include <stdint.h>

#include "ovkey.h"

// Code segments, functions and jump tables from the ROM bytes alone.
typedef struct Scan Scan;

// 4 KB page `page` of the ROM as 1024 host-order words; the pointer must stay valid until cs_free
typedef const uint32_t *(*cs_page_fn)(void *ctx, uint32_t page);

enum { CS_HOW_HEADER, CS_HOW_CALLS, CS_HOW_ICACHE, CS_HOW_TABLE, CS_HOW_ARGS };  // TABLE: an overlay, keyed (ovkey.h)
enum { CS_ORIG_CALL, CS_ORIG_ENTRY, CS_ORIG_POINTER, CS_ORIG_LUI, CS_ORIG_DATA, CS_ORIG_TILING, CS_ORIG_SWEEP };

typedef struct { uint32_t vram, rom, size, how, how_n; } CsSeg;
typedef struct { uint32_t start, end, origin, nunres; const uint32_t *unres; } CsFunc;
typedef struct { uint32_t jr, table, n; const uint32_t *targets; } CsTab;

typedef struct {
    uint32_t entry;
    uint32_t nseg, nfunc, ntab, nunplaced;
    CsSeg *seg;
    CsFunc *func;   // sorted by start
    CsTab *tab;     // by function, then by jr
    uint32_t *unplaced;
    uint32_t rounds;
    uint32_t *pool;  // the targets and unresolved lists, owned by the result
    uint32_t tables_short;  // cs_set_tables' pointer range missed a hull the scan used: rescan with cs_feed
} CsResult;

// What an IPL3 leaves behind: load shift below the header's entry, osCicId, SP memory fill and IMEM word 0, and the
// CIC seed its stage 2 is encrypted with (0: plain)
typedef struct { uint32_t shift, os_cic_id, sp_fill, imem0, seed; } CsIpl3;

// Where IPL3 copies ROM 0x1000; page0 is ROM page 0, and ipl3 (may be NULL) gets that IPL3's entry
uint32_t cs_boot_load(const uint32_t *page0, const CsIpl3 **ipl3);

Scan *cs_new(uint32_t rom_size, cs_page_fn page, void *ctx);
void cs_feed(Scan *s, const uint32_t *words, uint32_t n);  // every ROM word once, in order from offset 0
// Or the reader's whole-ROM pass: starts and pointer words sorted and unique as cs_feed collects them, ptrs only in
// [ptr_lo, ptr_hi); exact unless the scan looks outside that range, which sets CsResult.tables_short.
void cs_set_tables(Scan *s, const uint32_t *starts, uint32_t nstarts, const uint32_t *ptrs, uint32_t nptrs,
                   uint32_t ptr_lo, uint32_t ptr_hi);
const CsResult *cs_run(Scan *s);
// Run-time code instead of a ROM: s's ROM is the region's bytes from vram, fed in full, explored from seeds
const CsResult *cs_run_region(Scan *s, uint32_t vram, const uint32_t *seeds, uint32_t nseeds);
void cs_drop_work(Scan *s);  // frees everything but the result
void cs_free(Scan *s);

// Supplied by the platform
void *cs_alloc(size_t n);
void *cs_realloc(void *p, size_t n);
void cs_release(void *p);
void cs_die(const char *msg);
void cs_log(const char *fmt, ...);

#endif
