// A cart reader over Maple (firmware/n64, firmware/snes, ...): pipelined, CRC-checked block reads.
#include <kos.h>
#include <dc/maple.h>
#include <arch/irq.h>
#include <arch/memory.h>
#include <arch/timer.h>
#include <kos/mutex.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "cartlink.h"
#include "mapleraw.h"

#define CART_FUNC_N64 __builtin_bswap32(N64_CART_FUNC)  // as KOS reads it
#define CART_CHUNK    CART_CRC_MAX_LEN
#define WRITE_CHUNK   256u          // console-to-reader frames: 256 B answered, 512 B and up not (measured 2026-11-08)
#define STAGE         16384u
#define FRAMES        ((STAGE + CART_CHUNK - 1) / CART_CHUNK)
#define RX_BYTES      1024u         // a whole Maple reply: header and 255 words
#define RX_GUARD      1024u         // in maple_frame_t the next frame's queue links sit 16 bytes past the reply
#define GUARD_WORDS   16u
#define GUARD_FILL    0xA5C35A3Cu
#define FIND_MS       3000u
#define FETCH_MS      2000u
#define POWER_MS      5000u         // the reader flushes EEPROM writes for up to 2 s before the rail goes off

enum swap_mode { SWAP_NONE, SWAP_3210, SWAP_1032 };

static int g_port, g_unit;
static const uint32_t g_func = CART_FUNC_N64;  // the reader's function code, which every frame carries
static enum swap_mode g_swap;
static bool g_open;
static uint32_t g_crc_table[256];

typedef struct {
    maple_frame_t fr[FRAMES];
    uint32_t args[FRAMES][4];
    uint8_t rx[FRAMES][RX_BYTES + RX_GUARD] __attribute__((aligned(32)));  // only ever touched through P2
    uint8_t stage[STAGE];
} Work;

static Work *g_w;
static uint32_t g_base, g_len, g_space;  // g_space: an offset flag, 0 for the ROM
static volatile uint32_t g_done, g_inflight;
static volatile uint32_t g_not_ready, g_lost, g_crc_bad, g_bad;
static volatile uint32_t g_overruns, g_overrun_words, g_overrun_frame, g_overrun_hdr;
static volatile bool g_stop;

static uint32_t *rx_p2(unsigned i) {
    return (uint32_t *)(((uintptr_t)g_w->rx[i] & MEM_AREA_CACHE_MASK) | MEM_AREA_P2_BASE);
}

// After maple_frame_init: the reply goes to the frame's own slot, with a guard behind it
static void rx_arm(maple_frame_t *f, unsigned i) {
    uint32_t *rx = rx_p2(i);
    rx[0] = 0;
    for (unsigned k = 0; k < GUARD_WORDS; k++) rx[RX_BYTES / 4 + k] = GUARD_FILL;
    f->recv_buf = (uint8_t *)rx;
}

// IRQ context: a reply that ran into the guard
static void rx_check(unsigned i) {
    const uint32_t *rx = rx_p2(i), *g = rx + RX_BYTES / 4;
    unsigned n = GUARD_WORDS;
    while (n && g[n - 1] == GUARD_FILL) n--;
    if (!n) return;
    if (!g_overruns++) g_overrun_frame = i, g_overrun_hdr = rx[0];
    if (n > g_overrun_words) g_overrun_words = n;
}

static void overrun_report(void) {
    static uint32_t told;
    uint32_t n = g_overruns;
    if (!n || n < 2 * told) return;
    told = n;
    printf("cart: %lu Maple replies ran past %u bytes, up to %lu guard words (first: frame %lu, header %08lx)\n",
           (unsigned long)n, RX_BYTES, (unsigned long)g_overrun_words, (unsigned long)g_overrun_frame,
           (unsigned long)g_overrun_hdr);
}

// Words cross Maple byte-swapped relative to the firmware's put_word.
static inline uint32_t cart_word(uint32_t v) { return __builtin_bswap32(v); }

uint32_t cart_crc32(const void *p, uint32_t n) {
    const uint8_t *b = p;
    uint32_t c = 0xffffffffu;
    while (n--) c = g_crc_table[(c ^ *b++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

// Interrupts off. The hardware's DMA bit clears before the completion IRQ has run; a DMA started in that gap is
// taken as answered by the previous one's handler, which also lets KOS rebuild the descriptor list under it
static void flush_if_idle(void) {
    if (!maple_state.dma_in_progress && !maple_dma_in_progress()) maple_queue_flush();
}

static uint32_t chunk_len(unsigned i) {
    uint32_t at = i * CART_CHUNK;
    return g_len - at < CART_CHUNK ? g_len - at : CART_CHUNK;
}

static void issue(unsigned i);

// IRQ context: keep a chunk only when its echo, length and CRC all check out, otherwise ask again.
static void reply_cb(maple_state_t *st, maple_frame_t *f) {
    (void)st;
    unsigned i = (unsigned)(f - g_w->fr);
    const maple_response_t *r = (const maple_response_t *)f->recv_buf;
    const uint32_t *d = (const uint32_t *)r->data;
    uint32_t len = chunk_len(i);
    rx_check(i);
    maple_frame_unlock(f);

    if (r->response == MAPLE_RESPONSE_DATATRF && r->data_len == 3u + len / 4u && d[0] == g_func &&
        d[1] == g_w->args[i][1]) {
        if (cart_word(d[2 + len / 4]) == cart_crc32(d + 2, len)) {
            memcpy(g_w->stage + i * CART_CHUNK, d + 2, len);
            g_done++;
            g_inflight--;
            return;
        }
        g_crc_bad++;
    } else if (r->response == MAPLE_RESPONSE_FILEERR) {
        g_not_ready++;
    } else if (r->response == MAPLE_RESPONSE_NONE) {
        g_lost++;
    } else {
        g_bad++;
    }
    if (g_stop) {
        g_inflight--;
        return;
    }
    issue(i);
}

static void issue(unsigned i) {
    maple_frame_t *f = &g_w->fr[i];
    uint32_t *args = g_w->args[i];
    if (maple_frame_lock(f) < 0) {
        g_bad++;
        g_inflight--;
        return;
    }
    maple_frame_init(f);
    rx_arm(f, i);
    args[0] = g_func;
    args[1] = cart_word((g_base + i * CART_CHUNK) | g_space);
    args[2] = cart_word(chunk_len(i));
    args[3] = cart_word(CART_READ_CRC);
    f->cmd = MAPLE_COMMAND_BREAD;
    f->dst_port = g_port;
    f->dst_unit = g_unit;
    f->length = 4;
    f->send_buf = args;
    f->callback = reply_cb;
    maple_queue_frame(f);
}

// Let in-flight frames land, then pull anything still queued so no callback outlives the fetch.
static void drain(unsigned nframes) {
    g_stop = true;
    uint64_t t0 = timer_ms_gettime64();
    while (g_inflight && timer_ms_gettime64() - t0 < 500) thd_sleep(5);
    uint32_t old = irq_disable();
    for (unsigned i = 0; i < nframes; i++) {
        if (g_w->fr[i].queued) maple_queue_remove(&g_w->fr[i]);
        g_w->fr[i].state = MAPLE_FRAME_VACANT;
    }
    irq_restore(old);
}

// [base, base + len) into the stage, every chunk in flight at once; base and len are word multiples.
static bool fetch(uint32_t base, uint32_t len) {
    unsigned nframes = (len + CART_CHUNK - 1) / CART_CHUNK;
    g_base = base;
    g_len = len;
    g_done = 0;
    g_stop = false;
    g_inflight = nframes;
    uint64_t t0 = timer_ms_gettime64();
    for (unsigned i = 0; i < nframes; i++) issue(i);
    while (g_done < nframes && timer_ms_gettime64() - t0 < FETCH_MS) {
        uint32_t old = irq_disable();
        flush_if_idle();
        irq_restore(old);
        thd_pass();
    }
    bool ok = g_done >= nframes;
    drain(nframes);
    overrun_report();
    if (!ok)
        printf("cart: %u bytes at %06x incomplete after %u ms, %u/%u chunks; not-ready %u lost %u crc %u bad %u\n",
               (unsigned)len, (unsigned)base, FETCH_MS, (unsigned)g_done, nframes, (unsigned)g_not_ready,
               (unsigned)g_lost, (unsigned)g_crc_bad, (unsigned)g_bad);
    return ok;
}

static void to_z64(uint32_t len) {
    uint32_t *w = (uint32_t *)g_w->stage;
    if (g_swap == SWAP_3210)
        for (uint32_t i = 0; i < len / 4; i++) w[i] = __builtin_bswap32(w[i]);
    else if (g_swap == SWAP_1032)
        for (uint32_t i = 0; i < len / 4; i++) w[i] = ((w[i] & 0x00ff00ffu) << 8) | ((w[i] >> 8) & 0x00ff00ffu);
}

static bool l_read(uint32_t off, void *dst, uint32_t len) {
    uint8_t *out = dst;
    while (len) {
        uint32_t a0 = off & ~3u, skip = off - a0;
        uint32_t n = len < STAGE - skip ? len : STAGE - skip;
        if (!g_open || !fetch(a0, (skip + n + 3) & ~3u)) return false;
        to_z64((skip + n + 3) & ~3u);
        memcpy(out, g_w->stage + skip, n);
        out += n;
        off += n;
        len -= n;
    }
    return true;
}

// The cart's save chip (dc/eeptest.c's protocol): CART_SPACE_SAVE, CRC-checked both ways, synchronous.
#define SAVE_TRIES 8

static uint32_t l_save_size(void) {
    static mr_reply_t r;
    const uint32_t args[1] = { g_func };
    for (int t = 0; g_open && t < SAVE_TRIES; t++) {
        if (mr_xact(g_port, g_unit, MAPLE_COMMAND_GETMINFO, args, 1, &r) != 0) continue;
        uint32_t d[2];
        memcpy(d, r.data, sizeof d);
        if (r.response == MAPLE_RESPONSE_DATATRF && r.nbytes >= 8 && d[0] == g_func) return cart_word(d[1]);
    }
    return 0;
}

// GET_MEDIA_INFO's CART_MI_ROM_BYTES; 0 from firmware that predates it
static uint32_t l_rom_size(void) {
    static mr_reply_t r;
    const uint32_t args[1] = { g_func };
    for (int t = 0; g_open && t < SAVE_TRIES; t++) {
        if (mr_xact(g_port, g_unit, MAPLE_COMMAND_GETMINFO, args, 1, &r) != 0) continue;
        uint32_t d[2 + CART_MI_ROM_BYTES];  // the function code, then the words to the ROM size
        if (r.response != MAPLE_RESPONSE_DATATRF || r.nbytes < 8) continue;
        if (r.nbytes < sizeof d) return 0;
        memcpy(d, r.data, sizeof d);
        if (d[0] == g_func) return cart_word(d[1 + CART_MI_ROM_BYTES]);
    }
    return 0;
}

static bool save_read_chunk(uint32_t off, uint8_t *dst, uint32_t n) {
    static mr_reply_t r;
    const uint32_t args[4] = { g_func, cart_word(CART_SPACE_SAVE | off), cart_word(n), cart_word(CART_READ_CRC) };
    for (int t = 0; t < SAVE_TRIES; t++) {
        if (mr_xact(g_port, g_unit, MAPLE_COMMAND_BREAD, args, 4, &r) != 0) continue;
        if (r.response != MAPLE_RESPONSE_DATATRF || r.data_len != 3u + n / 4u) continue;
        uint32_t echo, crc;
        memcpy(&echo, r.data + 4, 4);
        memcpy(&crc, r.data + 8 + n, 4);
        if (echo != args[1] || cart_word(crc) != cart_crc32(r.data + 8, n)) continue;
        memcpy(dst, r.data + 8, n);
        return true;
    }
    return false;
}

static bool l_save_read(uint32_t off, void *dst, uint32_t len) {
    if (!g_open || ((off | len) & 3)) return false;
    for (uint32_t at = 0; at < len; at += CART_CHUNK)
        if (!save_read_chunk(off + at, (uint8_t *)dst + at, len - at < CART_CHUNK ? len - at : CART_CHUNK)) return false;
    return true;
}

// GET_MEDIA_INFO's words after the function code into w (n of them, CART_MI_*); false from firmware whose reply is shorter
static bool l_media_info(uint32_t *w, unsigned n) {
    static mr_reply_t r;
    const uint32_t args[1] = { g_func };
    for (int t = 0; g_open && t < SAVE_TRIES; t++) {
        if (mr_xact(g_port, g_unit, MAPLE_COMMAND_GETMINFO, args, 1, &r) != 0) continue;
        if (r.response != MAPLE_RESPONSE_DATATRF || r.nbytes < 8) continue;
        if (r.nbytes < 4 * (n + 1)) return false;
        uint32_t d[16];
        memcpy(d, r.data, 4 * (n + 1));
        if (d[0] != g_func) continue;
        for (unsigned i = 0; i < n; i++) w[i] = cart_word(d[1 + i]);
        return true;
    }
    return false;
}

// The reader's derived tables and cache spaces: pipelined like ROM reads, but no byte-order fix-up
static bool l_space_read(uint32_t space, uint32_t off, void *dst, uint32_t len) {
    if ((off | len) & 3) return false;
    uint8_t *out = dst;
    g_space = space;
    bool ok = true;
    while (len && ok) {
        uint32_t n = len < STAGE ? len : STAGE;
        ok = g_open && fetch(off, n);
        if (ok) memcpy(out, g_w->stage, n);
        out += n, off += n, len -= n;
    }
    g_space = 0;
    return ok;
}

// Writes are pipelined like reads, FRAMES at a time: one synchronous frame per Maple cycle is ~20 ms
static uint32_t g_wargs[FRAMES][3 + WRITE_CHUNK / 4], g_wn[FRAMES], g_wtries[FRAMES];
static volatile uint32_t g_wdone, g_wfail;

static void write_issue(unsigned i);

static void write_cb(maple_state_t *st, maple_frame_t *f) {
    (void)st;
    unsigned i = (unsigned)(f - g_w->fr);
    const maple_response_t *r = (const maple_response_t *)f->recv_buf;
    rx_check(i);
    maple_frame_unlock(f);
    if (r->response == MAPLE_RESPONSE_OK) {
        g_wdone++;
        g_inflight--;
        return;
    }
    if (g_stop || r->response == MAPLE_RESPONSE_FILEERR || ++g_wtries[i] >= SAVE_TRIES) {
        g_wfail++;
        g_inflight--;
        return;
    }
    write_issue(i);
}

static void write_issue(unsigned i) {
    maple_frame_t *f = &g_w->fr[i];
    if (maple_frame_lock(f) < 0) {
        g_wfail++;
        g_inflight--;
        return;
    }
    maple_frame_init(f);
    rx_arm(f, i);
    f->cmd = MAPLE_COMMAND_BWRITE;
    f->dst_port = g_port;
    f->dst_unit = g_unit;
    f->length = 3 + g_wn[i] / 4;
    f->send_buf = g_wargs[i];
    f->callback = write_cb;
    maple_queue_frame(f);
}

static bool l_space_write(uint32_t space, uint32_t off, const void *src, uint32_t len) {
    if (!g_open || ((off | len) & 3)) return false;
    for (uint32_t at = 0; at < len;) {
        unsigned nframes = 0;
        for (; nframes < FRAMES && at < len; nframes++, at += g_wn[nframes - 1]) {
            uint32_t n = len - at < WRITE_CHUNK ? len - at : WRITE_CHUNK;
            uint32_t *a = g_wargs[nframes];
            a[0] = g_func;
            a[1] = cart_word(space | (off + at));
            a[2] = cart_word(cart_crc32((const uint8_t *)src + at, n));
            memcpy(a + 3, (const uint8_t *)src + at, n);
            g_wn[nframes] = n, g_wtries[nframes] = 0;
        }
        g_wdone = g_wfail = 0;
        g_stop = false;
        g_inflight = nframes;
        uint64_t t0 = timer_ms_gettime64();
        for (unsigned i = 0; i < nframes; i++) write_issue(i);
        while (g_wdone + g_wfail < nframes && timer_ms_gettime64() - t0 < FETCH_MS) {
            uint32_t old = irq_disable();
            flush_if_idle();
            irq_restore(old);
            thd_pass();
        }
        bool ok = g_wdone == nframes;
        drain(nframes);
        overrun_report();
        if (!ok) {
            printf("cart: block writes at %08lx: %lu of %u taken\n", (unsigned long)(space | off), (unsigned long)g_wdone,
                   nframes);
            return false;
        }
    }
    return true;
}

static bool l_sync(uint32_t op, uint32_t a, uint32_t b) {
    static mr_reply_t r;
    const uint32_t args[4] = { g_func, cart_word(op), cart_word(a), cart_word(b) };
    for (int t = 0; g_open && t < SAVE_TRIES; t++) {
        if (mr_xact(g_port, g_unit, MAPLE_COMMAND_BSYNC, args, 4, &r) != 0) continue;
        if (r.response == MAPLE_RESPONSE_NONE) continue;
        return r.response == MAPLE_RESPONSE_OK;
    }
    return false;
}

bool cart_rom_is_z64(void) { return g_swap == SWAP_NONE; }

static bool l_save_write(uint32_t off, const void *src, uint32_t len) {
    static mr_reply_t r;
    uint32_t args[3 + 8];
    if (!g_open || len != 8 || (off & 7)) return false;
    args[0] = g_func;
    args[1] = cart_word(CART_SPACE_SAVE | off);
    args[2] = cart_word(cart_crc32(src, len));
    memcpy(args + 3, src, len);
    for (int t = 0; t < SAVE_TRIES; t++) {
        if (mr_xact(g_port, g_unit, MAPLE_COMMAND_BWRITE, args, 3 + (int)(len / 4), &r) != 0) continue;
        if (r.response == MAPLE_RESPONSE_NONE) continue;
        return r.response == MAPLE_RESPONSE_OK;
    }
    return false;
}

// Sub-units enumerate a scan after their parent, so keep asking until FIND_MS runs out.
static bool find_reader(void) {
    static mr_reply_t r;
    maple_wait_scan();
    uint64_t t0 = timer_ms_gettime64();
    do {
        for (int p = 0; p < 4; p++) {
            for (int u = 1; u <= 5; u++) {
                if (mr_xact(p, u, MAPLE_COMMAND_DEVINFO, NULL, 0, &r) != 0) continue;
                if (r.response != MAPLE_RESPONSE_DEVINFO || r.nbytes < 4) continue;
                uint32_t func;
                memcpy(&func, r.data, 4);
                if (func != g_func) continue;
                g_port = p;
                g_unit = u;
                printf("cart: reader at port %c unit %d\n", 'A' + p, u);
                return true;
            }
        }
        thd_sleep(250);
    } while (timer_ms_gettime64() - t0 < FIND_MS);
    return false;
}

static bool link_up(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
    g_open = false;
    if (!find_reader()) return false;
    if (!g_w && !(g_w = memalign(32, sizeof *g_w))) return false;
    memset(g_w, 0, sizeof *g_w);
    dcache_purge_range((uintptr_t)g_w, sizeof *g_w);  // no dirty line left to land over a reply the DMA wrote later
    g_open = true;
    return true;
}

static int learn_order(void) {
    g_swap = SWAP_NONE;
    uint8_t sig[4];
    if (!cart_read(0, sig, 4)) return CART_LINK_NO_CART;
    if (sig[0] != 0x80 && sig[0] != 0x40 && sig[0] != 0x37) {
        uint8_t far[4];  // the reader's read-ahead ring outlives a cart swap: seek away and back to refill it
        if (!cart_read(0x01000000, far, 4) || !cart_read(0, sig, 4)) return CART_LINK_NO_CART;
    }
    if (sig[0] == 0x40 && sig[1] == 0x12) g_swap = SWAP_3210;
    else if (sig[0] == 0x37 && sig[1] == 0x80) g_swap = SWAP_1032;
    else if (sig[0] != 0x80 || sig[1] != 0x37) {
        printf("cart: unknown ROM byte order %02x %02x %02x %02x\n", sig[0], sig[1], sig[2], sig[3]);
        return CART_LINK_NO_CART;
    }
    printf("cart: %s byte order\n", g_swap == SWAP_3210 ? "n64" : g_swap == SWAP_1032 ? "v64" : "z64");
    return CART_LINK_OK;
}

// Only the N64 reader's ROM has a byte order to learn; any other reader has a cart once it reports a ROM size.
static int l_probe(void) {
    if (!link_up()) return CART_LINK_NO_READER;
    return learn_order();
}

bool cart_link_open(void) { return cart_link_probe() == CART_LINK_OK; }

bool cart_link_is_open(void) { return g_open; }

// The reader refuses a power op while it is still carrying out the last one, so a second accept means it is done
static bool power_op(uint32_t op) {
    if (!g_open) return false;
    bool taken = false;
    uint64_t t0 = timer_ms_gettime64();
    while (timer_ms_gettime64() - t0 < POWER_MS) {
        if (cart_sync(op, 0, 0)) {
            if (taken) {
                printf("cart: power %s in %lu ms\n", op == CART_SYNC_POWER_OFF ? "off" : "on",
                       (unsigned long)(timer_ms_gettime64() - t0));
                return true;
            }
            taken = true;
        }
        thd_sleep(20);
    }
    printf("cart: power op %lu %s after %u ms\n", (unsigned long)op, taken ? "unfinished" : "never taken (old firmware?)",
           POWER_MS);
    return false;
}

static bool l_power_off(void) { return power_op(CART_SYNC_POWER_OFF); }

// The reader takes power-on as a no-op when the cart is already on, so this never reads a powered-off cart
static int l_power_on(void) {
    if (!link_up()) return CART_LINK_NO_READER;
    power_op(CART_SYNC_POWER_ON);
    return learn_order();
}

// Every entry point holds the link for its whole exchange: the play thread's ROM reads and a background upload share it
static mutex_t g_lock = RECURSIVE_MUTEX_INITIALIZER;
#define LOCKED(T, call) do { mutex_lock(&g_lock); T r_ = call; mutex_unlock(&g_lock); return r_; } while (0)

bool cart_read(uint32_t off, void *dst, uint32_t len) { LOCKED(bool, l_read(off, dst, len)); }
uint32_t cart_save_size(void) { LOCKED(uint32_t, l_save_size()); }
uint32_t cart_rom_size(void) { LOCKED(uint32_t, l_rom_size()); }
bool cart_save_read(uint32_t off, void *dst, uint32_t len) { LOCKED(bool, l_save_read(off, dst, len)); }
bool cart_save_write(uint32_t off, const void *src, uint32_t len) { LOCKED(bool, l_save_write(off, src, len)); }
bool cart_media_info(uint32_t *w, unsigned n) { LOCKED(bool, l_media_info(w, n)); }
bool cart_space_read(uint32_t space, uint32_t off, void *dst, uint32_t len) {
    LOCKED(bool, l_space_read(space, off, dst, len));
}
bool cart_space_write(uint32_t space, uint32_t off, const void *src, uint32_t len) {
    LOCKED(bool, l_space_write(space, off, src, len));
}
bool cart_sync(uint32_t op, uint32_t a, uint32_t b) { LOCKED(bool, l_sync(op, a, b)); }
int cart_link_probe(void) { LOCKED(int, l_probe()); }
bool cart_power_off(void) { LOCKED(bool, l_power_off()); }
int cart_power_on(void) { LOCKED(int, l_power_on()); }
