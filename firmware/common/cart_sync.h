// What a reader and the console say to each other; the console's dc/cartlink.h includes this file.
#pragma once

#define N64_CART_FUNC 0x00010000u  // ours; KOS reads its bswap32, 0x00000100, a bit it never assigns

// BLOCK_READ/WRITE {func, offset | space, ...}: a plain offset is the ROM
#define CART_SPACE_SAVE   0x80000000u
#define CART_SPACE_TABLES 0x40000000u  // tables the reader derived from the ROM
#define CART_SPACE_CACHE  0x20000000u  // the console's cache for this cart
#define CART_SPACE_XCACHE 0x10000000u  // the translation cache entry in flight (xcache.h)
#define CART_READ_CRC     0x01u        // BLOCK_READ flag: a CRC-32 (zlib) word follows the data
#define CART_CRC_MAX_LEN  1008u        // KOS's 255-word receive limit, less func, offset and the CRC

// BLOCK_SYNC {func, op, a, b}
enum {
    CART_SYNC_PASS = 1,         // the whole-ROM pass (n64/src/cart_pass.c)
    CART_SYNC_CACHE_COMMIT,     // {bytes, CRC-32}: the cache space's blob to the reader's flash
    CART_SYNC_POWER_OFF,
    CART_SYNC_POWER_ON,
    CART_SYNC_XC_LOOKUP,        // {key a, key b}: HIT (the entry streams into PSRAM) or MISS (common/xcache.c)
    CART_SYNC_XC_RESERVE,       // {bytes}: RECV, or FULL when it can't be stored
    CART_SYNC_XC_COMMIT,        // {bytes, CRC-32}: the first bytes written, at most the reserve, go to the card
};

enum { PASS_NONE, PASS_RUNNING, PASS_DONE, PASS_FAILED };
enum { COMMIT_NONE, COMMIT_BUSY, COMMIT_DONE, COMMIT_FAILED };
enum { XC_NONE, XC_IDLE, XC_BUSY, XC_HIT, XC_MISS, XC_RECV, XC_STORED, XC_FAILED, XC_FULL };

// The pass's tables space: a bitmap of the words that point into [PASS_PTR_LO, PASS_PTR_HI), then its jr ra starts
#define PASS_PTR_LO     0x80000000u
#define PASS_PTR_HI     0x80800000u
#define PASS_BITMAP     ((PASS_PTR_HI - PASS_PTR_LO) / 4 / 8)  // 256 KB
#define PASS_STARTS_OFF PASS_BITMAP

// GET_MEDIA_INFO's words after the function code; a reader's own words start at CART_MI_INFO
enum { CART_MI_SAVE_BYTES, CART_MI_PENDING, CART_MI_GIVEN_UP, CART_MI_ROM_BYTES, CART_MI_INFO };
enum { N64_MI_CACHE = CART_MI_INFO, N64_MI_PASS, N64_MI_STARTS, N64_MI_COMMIT, N64_MI_XC, N64_MI_XC_BYTES,
       N64_MI_SAVE_ID0, N64_MI_SAVE_ID1, N64_MI_END };  // SAVE_ID: the FlashRAM's id, 0 for an EEPROM
enum { MD_MI_SAVE = CART_MI_INFO, MD_MI_BUS, MD_MI_SWITCH };
#define CART_INFO(mi) ((mi) - CART_MI_INFO)  // its index in maple_cart_t.info's words
