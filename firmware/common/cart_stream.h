#pragma once
#include <stdbool.h>
#include <stdint.h>

// ROM blocks prefetched into RAM for the Maple cart unit: core0 fills, core1 serves.
#define CART_STREAM_BLOCK 512u      // one cart bus burst; the cart auto-increments only within one

typedef void (*cart_read_fn)(uint32_t addr, void *dst, uint32_t n);

typedef struct {
    uint32_t served, not_ready, seeks, filled_total;
    uint32_t read_us_max;   // one block off the cart bus
    uint32_t gap_us_max;    // longest wait between producer calls while the window had room
} cart_stream_stats_t;

// Fills the first window from block 0 before returning, so block 0 is servable at once.
// Calling it again drops the ring; core1 must not be reading it meanwhile.
void cart_stream_init(cart_read_fn read, uint32_t rom_base);

// Core0: reads at most one block. Call from the main loop whenever the bus is free.
void cart_stream_service(void);

// Core1: false while those ROM bytes are not in RAM yet; a far miss seeks the window there.
bool cart_stream_read(uint32_t off, uint32_t len, uint8_t *dst);

const cart_stream_stats_t *cart_stream_stats(void);
void cart_stream_clear_max(void);
