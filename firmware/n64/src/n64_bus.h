#pragma once
#include <stdbool.h>
#include <stdint.h>

// Core0. Claims PIO_CART_BUS and one DMA channel, and holds the cart in reset until it is released.
void n64_bus_init(void);
void n64_bus_set_reset(bool released);

#define N64_BUS_BURST 512u  // the cart auto-increments only within one aligned burst

// At most one N64_BUS_BURST, inside one. Lands big-endian as on the cart.
void n64_bus_read(uint32_t addr, void *dst, uint32_t n);
void n64_bus_read_words(uint32_t addr, uint16_t *out, uint32_t words);
void n64_bus_write_words(uint32_t addr, const uint16_t *words, uint32_t count);

// Console 'w': CPU-fed writes instead of DMA, to compare the two. Returns the new setting.
bool n64_bus_toggle_write_cpu(void);
