// Translated code in raw microSD sectors (docs/n64_any_cart.md, "SD cache"), keyed by cart id, size and two console
// words, staged one entry at a time in PSRAM; core0 does the SD work in xcache_service, core1 only copies.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cart_sync.h"
#include "console.h"

typedef struct {
    uint32_t index, stage;  // PSRAM offsets: the card's index mirrored (128 KB), and the entry in flight to the end
    void (*title)(const uint8_t id[64], char title[21]);  // a stored cart id as the status listing names it
} xcache_cfg_t;

void xcache_init(const xcache_cfg_t *cfg);              // core0, after boot: brings the card up and reads its index
void xcache_set_cart(const uint8_t id[64], uint32_t size);  // the cart's key: its first 64 bytes and ROM size
void xcache_service(void);                              // core0 main loop: a few sectors of a load or a store

// core1, inside the Maple reply turnaround
bool xcache_sync(uint32_t op, uint32_t a, uint32_t b);
uint32_t xcache_size(void);                             // HIT: the entry's bytes; reads past what is loaded wait
bool xcache_read(uint32_t off, uint32_t len, uint8_t *dst);
bool xcache_write(uint32_t off, uint32_t len, const uint8_t *src);
void xcache_info(uint32_t *state, uint32_t *bytes);     // bytes: the entry on HIT, else the most it can store

extern const console_cmd_t xcache_commands[];
extern const unsigned xcache_ncommands;
