// maple_cart.h — a cart reader as a Maple sub-unit: ROM reads, and save reads and writes, CRC-checked.
#pragma once
#include "maple_proto.h"
#include "cart_sync.h"

// BLOCK_READ {func, offset, [len], [flags]} -> {func, offset, data, [CRC-32]}; NOT_READY: ask again.
#define MAPLE_CART_DEFAULT_LEN  512u
#define MAPLE_CART_MAX_LEN      1012u           // KOS's 255-word receive limit, less func and offset
#define MAPLE_CART_SPACES       (CART_SPACE_SAVE | CART_SPACE_TABLES | CART_SPACE_CACHE | CART_SPACE_XCACHE)
#define MAPLE_CART_INFO_WORDS   6u

// FILE_ERR codes
#define MAPLE_CART_BAD_ARG      0x02u
#define MAPLE_CART_CRC_BAD      0x04u
#define MAPLE_CART_NOT_READY    0x10u

// GET_MEDIA_INFO {func} -> {func, save bytes, writes pending, blocks given up, ROM bytes, info()'s 6 words}; BLOCK_SYNC
// {func, op, a, b} -> ACK when sync() takes it; BLOCK_WRITE {func, offset | CART_SPACE_SAVE, CRC-32, data} -> ACK on accept.
typedef struct {                                // every callback runs on core1, inside the reply turnaround
    uint32_t rom_span;                          // ROM offsets at or past this are refused
    bool (*read)(uint32_t off, uint32_t len, uint8_t *dst);     // false: not in RAM yet
    uint32_t (*crc)(const uint8_t *p, uint32_t len);
    uint32_t (*save_size)(void);
    bool (*save_read)(uint32_t off, uint32_t len, uint8_t *dst);
    bool (*save_write)(uint32_t off, uint32_t len, const uint8_t *src);
    void (*save_status)(uint32_t *pending, uint32_t *given_up);
    uint32_t (*rom_size)(void);                 // the cart's ROM bytes; NULL reports rom_span
    // Optional: derived tables (read only) and a cache (read, write), addressed like the save by an offset flag
    uint32_t (*tables_size)(void);
    bool (*tables_read)(uint32_t off, uint32_t len, uint8_t *dst);  // false: not ready
    uint32_t (*cache_size)(void);
    bool (*cache_read)(uint32_t off, uint32_t len, uint8_t *dst);
    bool (*cache_write)(uint32_t off, uint32_t len, const uint8_t *src);
    uint32_t (*xcache_size)(void);
    bool (*xcache_read)(uint32_t off, uint32_t len, uint8_t *dst);  // false: not loaded yet
    bool (*xcache_write)(uint32_t off, uint32_t len, const uint8_t *src);
    bool (*sync)(uint32_t op, uint32_t a, uint32_t b);              // BLOCK_SYNC {func, op, a, b}, even while !ready
    void (*info)(uint32_t w[MAPLE_CART_INFO_WORDS]);                // the words from cart_sync.h's CART_MI_INFO
    bool (*ready)(void);                        // NULL: always; false refuses every cart command NOT_READY
} maple_cart_t;

// The console addresses it by id->func; cart and id must outlive the unit.
void maple_cart_unit(maple_unit_t *u, const maple_cart_t *cart, const maple_devid_t *id, uint8_t addr);
