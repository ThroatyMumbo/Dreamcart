#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cart_sync.h"

// The whole-ROM pass and the flash scan cache (docs/n64_any_cart.md); core0 does the bus and flash work.
#define CACHE_MAX       (PASS_BITMAP - 4096u)                  // a slot is its header sector and the blob

void cart_pass_init(void);      // core0, after boot: finds this cart's cache slot and loads it into the arena
void cart_pass_service(void);   // core0 main loop: a few ROM blocks of the pass, or one flash sector of a commit
void cart_pass_stop(void);      // core0, Maple refused: drops the pass and finishes a commit, keyed to this cart

// core1, inside the Maple reply turnaround
uint32_t cart_tables_size(void);                                  // 0 unless the pass is done
bool cart_tables_read(uint32_t off, uint32_t len, uint8_t *dst);
uint32_t cart_cache_size(void);                                   // the loaded or just written blob's bytes
bool cart_cache_read(uint32_t off, uint32_t len, uint8_t *dst);
bool cart_cache_write(uint32_t off, uint32_t len, const uint8_t *src);
bool cart_pass_sync(uint32_t op, uint32_t a, uint32_t b);        // CART_SYNC_PASS or CART_SYNC_CACHE_COMMIT
void cart_pass_info(uint32_t *w);                                 // info words N64_MI_CACHE to N64_MI_COMMIT
