#pragma once
#include <stdbool.h>
#include <stdint.h>

// The RAM copy and dirty map are sized from these; a build with a smaller chip overrides both.
#ifndef CART_SAVE_MAX
#define CART_SAVE_MAX   (128u << 10)
#endif
#ifndef CART_SAVE_BLOCK
#define CART_SAVE_BLOCK 256u    // the unit written to the chip and read back
#endif

// The cart's save chip as core0 reaches it; off + n never passes size.
typedef struct {
    uint32_t size;      // 0: no save
    bool (*read)(uint32_t off, uint8_t *dst, uint32_t n);          // false: the chip did not answer
    bool (*write)(uint32_t off, const uint8_t *src, uint32_t n);   // false: the chip refused the write
    bool (*busy)(void);             // NULL: write() finishes the cycle; else polled every poll_us until false
    uint32_t write_ms, poll_us;     // with busy: a cycle still busy after write_ms has failed
    bool (*load)(uint8_t *img, uint32_t size);  // NULL: two reads must agree; false: Maple sees no save
} cart_save_chip_t;

typedef struct { uint32_t writes, verify_fails, write_fails, given_up; } cart_save_stats_t;

// Core0, after the cart is identified: copies the save into RAM, served only if the chip reads back stable.
void cart_sram_init(const cart_save_chip_t *chip);

// Core0, Maple refused: drops the old cart's write state and loads the chip again, at its size now.
void cart_sram_reload(void);

// Core0 main loop: moves one dirty block a step through write, busy wait and read-back check.
void cart_sram_service(void);

// Core0: drains pending writes to the chip; false if some are still pending after ms.
bool cart_sram_flush(uint32_t ms);
cart_save_stats_t cart_sram_stats(void);

// Console 'e' and 'r': finish pending writes, then read the chip itself. The caller names the chip first.
void cart_sram_diagnose(void);
void cart_sram_dump(const char *tag);

// Core1, for the Maple cart unit: the RAM copy, which writes reach the cart from in the background.
uint32_t cart_sram_size(void);
bool cart_sram_read(uint32_t off, uint32_t len, uint8_t *dst);
bool cart_sram_write(uint32_t off, uint32_t len, const uint8_t *src);
// Core1: accepted writes not yet verified on the chip, and blocks given up on since boot.
void cart_sram_status(uint32_t *pending, uint32_t *given_up);
