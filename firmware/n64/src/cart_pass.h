#pragma once
#include <stdbool.h>
#include <stdint.h>

// The console's cart pipeline (recomp/harness/cartboot.c) leans on the reader for two things:
// - the whole-ROM pass at bus speed: every "jr ra" start (as recomp/cartscan's cs_feed collects them) and a
//   bitmap of the aligned KSEG0 words in [0x80000000, 0x80800000), one bit per possible pointer
// - a per-cart cache of the console's scan results in flash, keyed by the cart's header and size
// Both share one 256 KB arena; core1 only copies in and out of RAM, core0 does the bus and flash work.
#define PASS_PTR_LO     0x80000000u
#define PASS_PTR_HI     0x80800000u
#define PASS_BITMAP     ((PASS_PTR_HI - PASS_PTR_LO) / 4 / 8)  // 256 KB
#define PASS_STARTS_OFF PASS_BITMAP                            // in the tables space, after the bitmap
#define CACHE_MAX       (PASS_BITMAP - 4096u)                  // a slot is its header sector and the blob

enum { PASS_NONE, PASS_RUNNING, PASS_DONE, PASS_FAILED };
enum { COMMIT_NONE, COMMIT_BUSY, COMMIT_DONE, COMMIT_FAILED };

void cart_pass_init(void);      // core0, after boot: finds this cart's cache slot and loads it into the arena
void cart_pass_service(void);   // core0 main loop: a few ROM blocks of the pass, or one flash sector of a commit
void cart_pass_stop(void);      // core0, Maple refused: drops the pass and finishes a commit, keyed to this cart

// core1, inside the Maple reply turnaround
uint32_t cart_tables_size(void);                                  // 0 unless the pass is done
bool cart_tables_read(uint32_t off, uint32_t len, uint8_t *dst);
uint32_t cart_cache_size(void);                                   // the loaded or just written blob's bytes
bool cart_cache_read(uint32_t off, uint32_t len, uint8_t *dst);
bool cart_cache_write(uint32_t off, uint32_t len, const uint8_t *src);
bool cart_pass_sync(uint32_t op, uint32_t a, uint32_t b);        // op 1: start the pass; 2: commit a blob (len, crc)
void cart_pass_info(uint32_t *w);                                 // cache bytes, pass state, starts, commit state
