#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "md_map.h"

// Core0: reads the header, probes what the header can't say, and decides what Maple serves.
void md_cart_detect(void);
const md_cart_t *md_cart(void);
void md_cart_print(void);       // console 'i'

// Core0. The ROM in .bin file order, mirrored into served; 0xFF when nothing is servable. A cart_read_fn.
void md_cart_read(uint32_t off, void *dst, uint32_t n);

// Core0, as the cart_save_chip_t: off + n must not pass save_size; even off and n for a 16-bit save. Always true.
bool md_cart_save_read(uint32_t off, uint8_t *dst, uint32_t n);
bool md_cart_save_write(uint32_t off, const uint8_t *src, uint32_t n);
