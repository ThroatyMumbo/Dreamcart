#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "romcache.h"

#define BLOCK_SHIFT 12
#define BLOCK (1u << BLOCK_SHIFT)
#define MAX_BLOCKS 16384u  // 64 MB of ROM; reads past it go straight to the cart
#define MAX_SLOTS 4096u
#define RUN_BLOCKS 4u  // misses are fetched 16 KB at a time, the cart link's stage size

static uint16_t where[MAX_BLOCKS];  // slot + 1, or 0
static uint16_t owner[MAX_SLOTS];   // block + 1, or 0
static uint8_t *mem[MAX_SLOTS];
static uint8_t ref[MAX_SLOTS];
static uint32_t nslots, hand;
static uint8_t run_buf[RUN_BLOCKS * BLOCK] __attribute__((aligned(32)));
static romcache_fetch_fn fetch;
struct romcache_stats romcache_stats;

void romcache_init(romcache_fetch_fn f) { fetch = f; }

void romcache_add(void *p, uint32_t bytes) {
    uintptr_t a = ((uintptr_t)p + 31) & ~(uintptr_t)31, end = (uintptr_t)p + bytes;
    for (; a + BLOCK <= end && nslots < MAX_SLOTS; a += BLOCK) mem[nslots++] = (uint8_t *)a;
}

#define MAX_GIFTS 128u
static struct { void *p; uint32_t bytes; uint16_t s0, n; } gifts[MAX_GIFTS];  // malloc'd donations, oldest first
static uint32_t ngifts;

void romcache_add_heap(void *p, uint32_t bytes) {
    uint32_t s0 = nslots;
    romcache_add(p, bytes);
    if (ngifts < MAX_GIFTS) gifts[ngifts++] = (typeof(gifts[0])){ p, bytes, (uint16_t)s0, (uint16_t)(nslots - s0) };
}

uint32_t romcache_release(void) {
    if (!ngifts) return 0;
    uint32_t g = --ngifts, s0 = gifts[g].s0, n = gifts[g].n;
    for (uint32_t s = s0; s < s0 + n; s++)
        if (owner[s]) where[owner[s] - 1] = 0;
    for (uint32_t s = s0 + n; s < nslots; s++) {  // later donations (the reader's spare buffers) move down
        mem[s - n] = mem[s], owner[s - n] = owner[s], ref[s - n] = ref[s];
        if (owner[s - n]) where[owner[s - n] - 1] = (uint16_t)(s - n + 1);
    }
    for (uint32_t s = nslots - n; s < nslots; s++) owner[s] = 0, ref[s] = 0;
    nslots -= n;
    if (hand >= nslots) hand = 0;
    free(gifts[g].p);
    printf("romcache: %lu KB back to the heap, %lu KB left\n", (unsigned long)(gifts[g].bytes >> 10),
           (unsigned long)(romcache_bytes() >> 10));
    return gifts[g].bytes;
}

uint32_t romcache_bytes(void) { return nslots * BLOCK; }

// Clock: a slot read since the hand last passed it gets one more lap
static uint32_t victim(void) {
    for (;;) {
        uint32_t s = hand;
        hand = hand + 1 == nslots ? 0 : hand + 1;
        if (!ref[s]) return s;
        ref[s] = 0;
    }
}

static void install(uint32_t blk, const uint8_t *src) {
    uint32_t s = victim();
    if (owner[s]) where[owner[s] - 1] = 0;
    owner[s] = (uint16_t)(blk + 1);
    where[blk] = (uint16_t)(s + 1);
    ref[s] = 1;
    memcpy(mem[s], src, BLOCK);
}

int romcache_read(uint32_t off, void *dst, uint32_t len) {
    if (!nslots || off + len < off || off + len > MAX_BLOCKS * BLOCK) {
        romcache_stats.fetched_bytes += len;
        return fetch(off, dst, len);
    }
    uint8_t *out = dst;
    while (len) {
        uint32_t blk = off >> BLOCK_SHIFT, in = off & (BLOCK - 1), n = BLOCK - in < len ? BLOCK - in : len;
        const uint8_t *src;
        if (where[blk]) {
            uint32_t s = where[blk] - 1u;
            ref[s] = 1;
            src = mem[s];
            romcache_stats.hit_bytes += n;
        } else {
            uint32_t last = (off + len - 1) >> BLOCK_SHIFT, run = 1;
            while (run < RUN_BLOCKS && blk + run <= last && !where[blk + run]) run++;
            if (!fetch(blk << BLOCK_SHIFT, run_buf, run << BLOCK_SHIFT)) return 0;
            romcache_stats.fetched_bytes += run << BLOCK_SHIFT;
            for (uint32_t i = 0; i < run; i++) install(blk + i, run_buf + (i << BLOCK_SHIFT));
            src = run_buf;  // a tiny cache may already have evicted the slot
        }
        memcpy(out, src + in, n);
        out += n;
        off += n;
        len -= n;
    }
    return 1;
}
