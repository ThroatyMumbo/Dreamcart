#include <stddef.h>
#include <string.h>
#include "cart_stream.h"
#include "hardware/sync.h"
#include "pico/time.h"

#define BLK    CART_STREAM_BLOCK
#define RING   192u
#define SLACK  64u  // blocks kept behind the newest served one: 24 frames of 1012 B in flight, all retried

static uint8_t ring[RING][BLK] __attribute__((aligned(4)));

// core0 writes base, filled, seek_ack; core1 writes served, seek_blk, seek_req
static volatile uint32_t base, filled, seek_ack;
static volatile uint32_t served, seek_blk, seek_req;

#define AHEAD_SEEK_US 2000u     // no serve for this long: the far request is the only one left, seek to it
static uint32_t g_last_serve_us;    // core1

static cart_read_fn g_read;
static uint32_t g_rom_base;
static cart_stream_stats_t g_st;

static void seek(uint32_t blk) {
    served = 0;
    seek_blk = blk;
    __dmb();
    seek_req = seek_req + 1;
    g_st.seeks++;
}

// k counts blocks from base; a slot is never rewritten while k + SLACK >= served.
bool cart_stream_read(uint32_t off, uint32_t len, uint8_t *dst) {
    if (seek_ack != seek_req) { g_st.not_ready++; return false; }
    uint32_t b0 = off / BLK, b1 = (off + len - 1) / BLK;
    uint32_t have = filled;
    __dmb();
    if (b0 >= base && b0 - base + SLACK >= served) {
        uint32_t k0 = b0 - base, k1 = b1 - base;
        if (k1 < have) {
            if (k1 >= served) served = k1 + 1;
            for (uint32_t k = k0, at = off % BLK; k <= k1; k++, at = 0) {
                uint32_t n = BLK - at < len ? BLK - at : len;
                memcpy(dst, ring[k % RING] + at, n);
                dst += n;
                len -= n;
            }
            g_st.served++;
            g_last_serve_us = time_us_32();
            return true;
        }
        if (k1 < served + RING - SLACK) { g_st.not_ready++; return false; }
        // Ahead of the window: older requests drain first, or a stuck one and a far one seek-ping-pong forever.
        if (time_us_32() - g_last_serve_us < AHEAD_SEEK_US) { g_st.not_ready++; return false; }
    }
    seek(b0);
    g_st.not_ready++;
    return false;
}

void cart_stream_service(void) {
    static uint32_t last_us;
    uint32_t now = time_us_32();
    uint32_t req = seek_req;
    if (seek_ack != req) {
        __dmb();
        base = seek_blk;
        filled = 0;
        __dmb();
        seek_ack = req;
        last_us = now;
        return;
    }
    uint32_t k = filled;
    if (k >= served + RING - SLACK) { last_us = 0; return; }
    if (last_us && now - last_us > g_st.gap_us_max) g_st.gap_us_max = now - last_us;
    g_read(g_rom_base + (base + k) * BLK, ring[k % RING], BLK);
    __dmb();
    filled = k + 1;
    g_st.filled_total++;
    last_us = time_us_32();
    if (last_us - now > g_st.read_us_max) g_st.read_us_max = last_us - now;
}

void cart_stream_init(cart_read_fn read, uint32_t rom_base) {
    g_read = read;
    g_rom_base = rom_base;
    base = filled = served = 0;
    seek_ack = seek_req;
    for (uint32_t i = 0; i < RING - SLACK; i++) cart_stream_service();
}

const cart_stream_stats_t *cart_stream_stats(void) { return &g_st; }

void cart_stream_clear_max(void) { g_st.read_us_max = g_st.gap_us_max = 0; }
