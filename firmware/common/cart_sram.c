#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "console.h"
#include "cart_sram.h"

#define BLK     CART_SAVE_BLOCK
#define BLOCKS  (CART_SAVE_MAX / BLK)
#define RETRIES 3u

static const cart_save_chip_t *g_chip;
static uint8_t g_img[CART_SAVE_MAX];
static uint32_t g_size;
static bool g_unclean;
static volatile uint8_t g_dirty[BLOCKS];        // core1 sets, core0 clears before it copies the block
static volatile bool g_hint;                    // core1: something may be dirty
static volatile uint32_t g_accepted;            // core1: Maple writes taken into the RAM copy
static volatile uint32_t g_flushed;             // core0: g_accepted as of the last scan that found nothing dirty
static volatile uint32_t g_given_up;            // core0: blocks dropped after RETRIES

// The block in flight; g_busy while a chip with a busy poll runs its write cycle
static bool g_busy;
static uint32_t g_block, g_len, g_tries;
static uint8_t g_data[BLK];
static absolute_time_t g_deadline, g_next_poll;
static uint32_t g_writes, g_verify_fails, g_write_fails;

static bool read_twice(uint32_t size) {
    static uint8_t again[BLK];
    if (!g_chip->read(0, g_img, size)) return false;
    for (uint32_t off = 0; off < size; off += BLK) {
        uint32_t n = size - off < BLK ? size - off : BLK;
        if (!g_chip->read(off, again, n) || memcmp(again, g_img + off, n)) {
            printf("save: two reads differ at %05lX; Maple sees no save\n", (unsigned long)off);
            return false;
        }
    }
    return true;
}

static void load(void) {
    uint32_t size = g_chip->size;
    g_size = 0;
    g_unclean = false;
    if (!size || size > CART_SAVE_MAX) return;
    if (g_chip->load ? !g_chip->load(g_img, size) : !read_twice(size)) {
        g_unclean = true;
        return;
    }
    g_size = size;
}

void cart_sram_init(const cart_save_chip_t *chip) {
    g_chip = chip;
    load();
}

void cart_sram_reload(void) {
    memset((void *)g_dirty, 0, sizeof g_dirty);
    g_hint = false;
    g_busy = false;
    g_tries = 0;
    g_flushed = g_accepted;
    if (g_chip) load();
}

static void retry_or_give_up(void) {
    if (++g_tries < RETRIES) {
        g_dirty[g_block] = 1;
        g_hint = true;
    } else {
        g_given_up++;
        g_tries = 0;
    }
    g_busy = false;
}

static void verify(void) {
    static uint8_t back[BLK];
    if (!g_chip->read(g_block * BLK, back, g_len) || memcmp(back, g_data, g_len)) {
        g_verify_fails++;
        retry_or_give_up();
        return;
    }
    g_tries = 0;
    g_busy = false;
}

// No hint means the last scan, which began after every accepted write's marks, found nothing.
static void start_next(void) {
    uint32_t accepted = g_accepted;
    __dmb();
    if (!g_hint) { g_flushed = accepted; return; }
    g_hint = false;
    __dmb();
    for (uint32_t b = 0; b < (g_size + BLK - 1) / BLK; b++) {
        if (!g_dirty[b]) continue;
        g_dirty[b] = 0;
        __dmb();
        if (b != g_block) g_tries = 0;      // a retry of the same block keeps its count
        g_block = b;
        g_len = g_size - b * BLK < BLK ? g_size - b * BLK : BLK;
        memcpy(g_data, g_img + b * BLK, g_len);
        g_hint = true;      // look again after this one
        if (!g_chip->write(b * BLK, g_data, g_len)) {
            g_write_fails++;
            retry_or_give_up();
            return;
        }
        g_writes++;
        if (!g_chip->busy) {
            verify();
            return;
        }
        g_busy = true;
        g_deadline = make_timeout_time_ms(g_chip->write_ms);
        g_next_poll = make_timeout_time_us(g_chip->poll_us);
        return;
    }
}

static void poll(void) {
    if (!time_reached(g_next_poll)) return;
    g_next_poll = make_timeout_time_us(g_chip->poll_us);
    if (!g_chip->busy()) {
        verify();
    } else if (time_reached(g_deadline)) {
        g_write_fails++;
        retry_or_give_up();
    }
}

void cart_sram_service(void) {
    if (g_busy) {
        poll();
        return;
    }
    start_next();
    while (g_tries && !g_chip->busy) start_next();     // without a busy poll, a block's retries run in one call
}

bool cart_sram_flush(uint32_t ms) {
    absolute_time_t until = make_timeout_time_ms(ms);
    while ((g_busy || g_hint) && !time_reached(until)) cart_sram_service();
    return !g_busy && !g_hint;
}

cart_save_stats_t cart_sram_stats(void) {
    return (cart_save_stats_t){ g_writes, g_verify_fails, g_write_fails, g_given_up };
}

void cart_sram_diagnose(void) {
    if (!g_chip || !g_chip->size) return;
    bool done = cart_sram_flush(2000);
    printf("%s%lu block writes, %lu verify fails, %lu given up%s\n",
           g_unclean ? "NOT SERVED: boot reads differed; " : "", (unsigned long)g_writes,
           (unsigned long)g_verify_fails, (unsigned long)g_given_up, done ? "" : ", writes still pending");
    static uint8_t data[256];
    uint32_t n = g_chip->size < sizeof data ? g_chip->size : sizeof data;
    g_chip->read(0, data, n);
    console_hexdump(data, n);
}

static void chip_read(uint32_t off, void *dst, uint32_t n) { g_chip->read(off, dst, n); }

void cart_sram_dump(const char *tag) {
    uint32_t size = g_chip ? g_chip->size : 0;
    if (!size) { printf("refused: no save\n"); return; }
    console_raw_dump(tag, size, chip_read);
}

uint32_t cart_sram_size(void) { return g_size; }

bool cart_sram_read(uint32_t off, uint32_t len, uint8_t *dst) {
    if (off > g_size || len > g_size - off) return false;
    memcpy(dst, g_img + off, len);
    return true;
}

bool cart_sram_write(uint32_t off, uint32_t len, const uint8_t *src) {
    if (!len || off > g_size || len > g_size - off) return false;
    memcpy(g_img + off, src, len);
    __dmb();
    for (uint32_t b = off / BLK; b <= (off + len - 1) / BLK; b++) g_dirty[b] = 1;
    __dmb();
    g_hint = true;
    __dmb();
    g_accepted = g_accepted + 1;
    return true;
}

void cart_sram_status(uint32_t *pending, uint32_t *given_up) {
    *given_up = g_given_up;
    *pending = g_accepted - g_flushed;
}
