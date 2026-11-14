#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "snes_map.h"

// Core0: reads the header at 00:FFB0 and decides what Maple serves.
void snes_cart_detect(void);
const snes_cart_t *snes_cart(void);
void snes_cart_print(void);     // console 'i'

// Core0. The ROM in .sfc file order, mirrored into rom_size; 0xFF when nothing is servable. A cart_read_fn.
void snes_cart_read(uint32_t off, void *dst, uint32_t n);

// Core0, as the cart_save_chip_t: off + n must not pass sram_size. Always true; the bus has no answer to check.
bool snes_cart_sram_read(uint32_t off, uint8_t *dst, uint32_t n);
bool snes_cart_sram_write(uint32_t off, const uint8_t *src, uint32_t n);
