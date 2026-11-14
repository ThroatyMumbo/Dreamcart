// The console's translated code, cached on the microSD card in raw sectors (docs/n64_any_cart.md, "SD cache").
// Entries are keyed by the cart (64-byte id and size) and two words from the console; one entry at a time is
// staged in PSRAM. Core0 does the SD work in xcache_service; core1 only copies to and from PSRAM.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "console.h"

#define XC_SYNC_LOOKUP  5u      // BLOCK_SYNC {op, key a, key b}: HIT (entry streams into PSRAM) or MISS
#define XC_SYNC_RESERVE 6u      // {op, bytes}: RECV, or FULL when it can't be stored
#define XC_SYNC_COMMIT  7u      // {op, bytes, CRC-32}: the first bytes written, at most the reserve, go to the card

enum { XC_NONE, XC_IDLE, XC_BUSY, XC_HIT, XC_MISS, XC_RECV, XC_STORED, XC_FAILED, XC_FULL };

void xcache_init(void);                                 // core0, after boot: brings the card up and reads its index
void xcache_set_cart(const uint8_t id[64], uint32_t size);
void xcache_service(void);                              // core0 main loop: a few sectors of a load or a store

// core1, inside the Maple reply turnaround
bool xcache_sync(uint32_t op, uint32_t a, uint32_t b);
uint32_t xcache_size(void);                             // HIT: the entry's bytes; reads past what is loaded wait
bool xcache_read(uint32_t off, uint32_t len, uint8_t *dst);
bool xcache_write(uint32_t off, uint32_t len, const uint8_t *src);
void xcache_info(uint32_t *state, uint32_t *bytes);     // bytes: the entry on HIT, else the most it can store

extern const console_cmd_t xcache_commands[];
extern const unsigned xcache_ncommands;
