#pragma once
#include <stdbool.h>
#include <stdint.h>

#define ROM_MB_MAX 64u
#define ROM_LIMIT  (ROM_MB_MAX << 20)

#define N64_MAGIC    0x80371240u
#define CART_HDR_LEN 64u

// The cart's first 64 bytes, as on the bus; its magic word, and its 20-byte title less trailing spaces.
void cart_header_read(uint8_t h[CART_HDR_LEN]);
uint32_t cart_header_magic(const uint8_t *h);
void cart_header_title(const uint8_t *h, char title[21]);

// The size the diagnostics work within: ROM_MB_MAX until 'i' matches a known cart, 'm' sets it or 'z' probes it.
uint32_t cart_rom_mb(void);
bool cart_set_rom_mb(uint32_t mb);      // false outside 1..ROM_MB_MAX
static inline uint32_t cart_rom_bytes(void) { return cart_rom_mb() << 20; }
uint32_t cart_rom_size(void);           // the same, for the Maple unit's GET_MEDIA_INFO

// Console 'i': header dump, byte-order check, title, and a known-cart match that sets the ROM size.
void cart_info_print(void);
