#include "cart_pass.h"

#include <stddef.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/platform.h"
#include "board.h"
#include "n64_bus.h"
#include "cart_info.h"
#include "crc32.h"
#include "psram.h"
#include "xcache.h"

#define JR_RA        0x03E00008u
#define MAX_PAD      8
#define MAX_STARTS   12288u
#define BLOCK        N64_BUS_BURST
#define BLOCKS_TURN  8u           // per main-loop turn, so the ROM ring still refills between them
#define SLOTS        4u
#define SLOT_BYTES   (256u << 10)
#define SLOT_BASE    (PICO_FLASH_SIZE_BYTES - SLOTS * SLOT_BYTES)  // the top 1 MB; openocd leaves it alone
#define SECTOR       4096u
#define SLOT_MAGIC   0x31534343u  // "CCS1"

typedef struct {
    uint32_t magic, seq, rom_size, len, crc;
    uint8_t header[64];           // the cart's first 64 bytes, as on the bus
    uint32_t self_crc;
} slot_hdr_t;

static uint32_t g_arena[PASS_BITMAP / 4];   // the pass's bitmap, or a cache blob
static uint32_t *const g_starts = (uint32_t *)(PSRAM_BASE + PSRAM_PASS_STARTS);   // MAX_STARTS words
_Static_assert(PSRAM_PASS_STARTS + 4 * MAX_STARTS <= PSRAM_XC_INDEX, "the pass's starts run into the SD cache's index");
static uint8_t g_cart_hdr[64];

static volatile uint32_t g_pass = PASS_NONE, g_pass_req, g_nstarts;
static volatile uint32_t g_cache_len;       // valid bytes of blob in the arena, 0 while it holds anything else
static volatile uint32_t g_commit = COMMIT_NONE, g_commit_req, g_commit_len, g_commit_crc;

// The pass, between turns
static uint32_t g_pos, g_rom_size, g_pend_p[16], g_pend_n[16], g_npend;
// A commit, between turns
static uint32_t g_slot, g_step, g_seq;


static const slot_hdr_t *slot_at(uint32_t i) { return (const slot_hdr_t *)(XIP_BASE + SLOT_BASE + i * SLOT_BYTES); }

static bool slot_valid(const slot_hdr_t *h) {
    return h->magic == SLOT_MAGIC && h->len && h->len <= CACHE_MAX &&
           h->self_crc == crc32_update(0, h, offsetof(slot_hdr_t, self_crc));
}

static bool slot_is_this_cart(const slot_hdr_t *h) {
    return slot_valid(h) && h->rom_size == cart_rom_size() && !memcmp(h->header, g_cart_hdr, 64);
}

void cart_pass_init(void) {
    g_cache_len = 0;
    g_pass = PASS_NONE, g_pass_req = 0, g_nstarts = 0;
    g_commit = COMMIT_NONE, g_commit_req = 0;
    cart_header_read(g_cart_hdr);
    for (uint32_t i = 0; i < SLOTS; i++) {
        const slot_hdr_t *h = slot_at(i);
        if (h->seq >= g_seq && slot_valid(h)) g_seq = h->seq + 1;
    }
    for (uint32_t i = 0; i < SLOTS; i++) {
        const slot_hdr_t *h = slot_at(i);
        const uint8_t *blob = (const uint8_t *)h + SECTOR;
        if (slot_is_this_cart(h) && crc32_update(0, blob, h->len) == h->crc) {
            memcpy(g_arena, blob, h->len);
            __dmb();
            g_cache_len = h->len;
            return;
        }
    }
}

// cs_feed's rules exactly: a start is 8 bytes past a jr ra, moved past up to 8 zero words
static void start_found(uint32_t k) {
    if (g_nstarts == MAX_STARTS) { g_pass = PASS_FAILED; return; }
    g_starts[g_nstarts++] = g_pend_p[k];
    g_pend_p[k] = g_pend_p[g_npend - 1], g_pend_n[k] = g_pend_n[g_npend - 1];
    g_npend--;
}

static void pass_word(uint32_t off, uint32_t x) {
    for (uint32_t k = 0; k < g_npend;) {
        if (g_pend_p[k] != off) { k++; continue; }
        if (x != 0) { start_found(k); continue; }
        g_pend_p[k] += 4;
        if (++g_pend_n[k] == MAX_PAD) { start_found(k); continue; }
        k++;
    }
    if (x == JR_RA) {
        if (g_npend == 16) { g_pass = PASS_FAILED; return; }
        g_pend_p[g_npend] = off + 8, g_pend_n[g_npend++] = 0;
    }
    if (!(x & 3) && x >= PASS_PTR_LO && x < PASS_PTR_HI) {
        uint32_t bit = (x - PASS_PTR_LO) >> 2;
        g_arena[bit >> 5] |= 1u << (bit & 31);
    }
}

static void pass_step(void) {
    static uint8_t buf[BLOCK];
    for (uint32_t b = 0; b < BLOCKS_TURN && g_pass == PASS_RUNNING; b++) {
        if (g_pos >= g_rom_size) {
            while (g_npend && g_pass == PASS_RUNNING) start_found(0);  // past the ROM the word is not zero
            if (g_pass != PASS_RUNNING) return;
            for (uint32_t i = 1; i < g_nstarts; i++) {  // already in order; cs_set_tables requires it, so make sure
                uint32_t v = g_starts[i], j = i;
                while (j > 0 && g_starts[j - 1] > v) g_starts[j] = g_starts[j - 1], j--;
                g_starts[j] = v;
            }
            __dmb();
            g_pass = PASS_DONE;
            return;
        }
        n64_bus_read(CART_ROM + g_pos, buf, BLOCK);
        for (uint32_t i = 0; i < BLOCK; i += 4)
            pass_word(g_pos + i, (uint32_t)buf[i] << 24 | buf[i + 1] << 16 | buf[i + 2] << 8 | buf[i + 3]);
        g_pos += BLOCK;
    }
}

static bool flash_ok(uint32_t off, const void *src, uint32_t n) {
    return !memcmp((const void *)(XIP_BASE + off), src, n);
}

// Erases n bytes at off when src is NULL, else programs them from src
static void flash_op(uint32_t off, const uint8_t *src, uint32_t n) {
    psram_lock();
    uint32_t irq = save_and_disable_interrupts();
    if (src) flash_range_program(off, src, n);
    else flash_range_erase(off, n);
    restore_interrupts(irq);
    psram_unlock();
}

// Erase the slot's sectors one per turn, program the blob, then its header: a torn commit leaves no valid header
static void commit_step(void) {
    uint32_t base = SLOT_BASE + g_slot * SLOT_BYTES, blob_sectors = (g_commit_len + SECTOR - 1) / SECTOR;
    if (g_step < 1 + blob_sectors) {
        flash_op(base + g_step * SECTOR, NULL, SECTOR);
    } else if (g_step < 1 + 2 * blob_sectors) {
        uint32_t s = g_step - 1 - blob_sectors, off = s * SECTOR;
        static uint8_t page[SECTOR];
        uint32_t n = g_commit_len - off < SECTOR ? g_commit_len - off : SECTOR;
        memset(page, 0xFF, sizeof page);
        memcpy(page, (const uint8_t *)g_arena + off, n);
        flash_op(base + SECTOR + off, page, SECTOR);
        if (!flash_ok(base + SECTOR + off, page, SECTOR)) { g_commit = COMMIT_FAILED; return; }
    } else {
        static uint8_t page[FLASH_PAGE_SIZE];
        slot_hdr_t h = {SLOT_MAGIC, g_seq++, cart_rom_size(), g_commit_len, g_commit_crc, {0}, 0};
        memcpy(h.header, g_cart_hdr, 64);
        h.self_crc = crc32_update(0, &h, offsetof(slot_hdr_t, self_crc));
        memset(page, 0xFF, sizeof page);
        memcpy(page, &h, sizeof h);
        flash_op(base, page, FLASH_PAGE_SIZE);
        __dmb();
        if (!flash_ok(base, &h, sizeof h)) { g_commit = COMMIT_FAILED; return; }
        g_cache_len = g_commit_len;
        g_commit = COMMIT_DONE;
        return;
    }
    g_step++;
}

static void commit_begin(void) {
    if (crc32_update(0, g_arena, g_commit_len) != g_commit_crc) { g_commit = COMMIT_FAILED; return; }
    uint32_t pick = SLOTS, oldest = 0;
    for (uint32_t i = 0; i < SLOTS && pick == SLOTS; i++)
        if (slot_is_this_cart(slot_at(i)) || !slot_valid(slot_at(i))) pick = i;
    for (uint32_t i = 0; i < SLOTS && pick == SLOTS; i++)
        if (slot_at(i)->seq < slot_at(oldest)->seq) oldest = i;
    g_slot = pick == SLOTS ? oldest : pick;
    g_step = 0;
    g_commit = COMMIT_BUSY;
}

void cart_pass_service(void) {
    if (g_pass_req) {
        g_pass_req = 0;
        g_cache_len = 0;
        memset(g_arena, 0, sizeof g_arena);
        g_pos = 0, g_rom_size = cart_rom_size(), g_npend = 0, g_nstarts = 0;
        __dmb();
        g_pass = psram_size() >= PSRAM_PASS_STARTS + 4 * MAX_STARTS ? PASS_RUNNING : PASS_FAILED;
    }
    if (g_pass == PASS_RUNNING) pass_step();
    if (g_commit_req) {
        g_commit_req = 0;
        commit_begin();
    }
    if (g_commit == COMMIT_BUSY) commit_step();
}

void cart_pass_stop(void) {
    g_pass_req = 0;
    if (g_pass == PASS_RUNNING) g_pass = PASS_NONE;
    if (g_commit_req) {
        g_commit_req = 0;
        commit_begin();
    }
    while (g_commit == COMMIT_BUSY) commit_step();
}

uint32_t cart_tables_size(void) { return g_pass == PASS_DONE ? PASS_STARTS_OFF + 4 * g_nstarts : 0; }

bool __not_in_flash_func(cart_tables_read)(uint32_t off, uint32_t len, uint8_t *dst) {
    if (g_pass != PASS_DONE) return false;
    uint32_t end = off + len, b = PASS_STARTS_OFF;
    if (off < b) memcpy(dst, (const uint8_t *)g_arena + off, (end < b ? end : b) - off);
    if (end > b) {
        uint32_t s = off > b ? off : b;
        if (!psram_enter()) return false;
        memcpy(dst + (s - off), (const uint8_t *)g_starts + (s - b), end - s);
        psram_leave();
    }
    return true;
}

uint32_t cart_cache_size(void) { return g_cache_len; }

bool __not_in_flash_func(cart_cache_read)(uint32_t off, uint32_t len, uint8_t *dst) {
    if (!g_cache_len || off + len > g_cache_len) return false;
    memcpy(dst, (const uint8_t *)g_arena + off, len);
    return true;
}

// Written while nothing else owns the arena: the console has read the tables it asked for
bool __not_in_flash_func(cart_cache_write)(uint32_t off, uint32_t len, const uint8_t *src) {
    if (g_pass == PASS_RUNNING || g_commit == COMMIT_BUSY || g_commit_req || off + len > CACHE_MAX) return false;
    g_cache_len = 0;
    if (g_pass == PASS_DONE) g_pass = PASS_NONE;
    memcpy((uint8_t *)g_arena + off, src, len);
    return true;
}

bool __not_in_flash_func(cart_pass_sync)(uint32_t op, uint32_t a, uint32_t b) {
    if (g_pass == PASS_RUNNING || g_commit == COMMIT_BUSY || g_commit_req || g_pass_req) return false;
    if (op == CART_SYNC_PASS) {
        g_commit = COMMIT_NONE;
        __dmb();
        g_pass_req = 1;
        return true;
    }
    if (op == CART_SYNC_CACHE_COMMIT && a && a <= CACHE_MAX) {
        g_commit_len = a, g_commit_crc = b;
        __dmb();
        g_commit_req = 1;
        return true;
    }
    return false;
}

void __not_in_flash_func(cart_pass_info)(uint32_t *w) {
    w[CART_INFO(N64_MI_CACHE)] = g_cache_len;
    w[CART_INFO(N64_MI_PASS)] = g_pass_req ? PASS_RUNNING : g_pass;
    w[CART_INFO(N64_MI_STARTS)] = g_nstarts;
    w[CART_INFO(N64_MI_COMMIT)] = g_commit_req ? COMMIT_BUSY : g_commit;
}
