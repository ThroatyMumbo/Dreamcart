// cartboot's CartSrc over the cart link: firmware/n64/src/cart_pass.h and firmware/common/xcache.h on the other side
#include <kos.h>
#include <malloc.h>
#include <string.h>
#include "cartsrc_reader.h"
#include "cartlink.h"
#include "xlpack.h"
#include "xlz.h"

#define MI_N  N64_MI_XC  // the pass's and the flash cache's words
#define MI_XN N64_MI_END
#define XC_STEP 16384u
#define BG_STEP 4096u  // one batch of write frames: the play thread waits at most this long for the link

static const XlOut *g_boot_out;
static uint32_t rd_calls, rd_bytes;
static uint64_t rd_us;
static int xc_quiet;  // said once that there is no SD cache
volatile uint32_t reader_xc_done, reader_xc_total;

// One entry moves at a time (overlays skip the cache while the boot entry uploads), so these are shared
static uint8_t xc_buf[XC_STEP] __attribute__((aligned(4)));
static uint8_t xc_rec[XLP_REC_MAX] __attribute__((aligned(4))), xc_scratch[XLP_BLOCK] __attribute__((aligned(4)));
static uint16_t xc_work[XLZ_WORK];
static XlpLoad xc_load;

// The boot translation's entry goes to the reader once the game runs. tload leaves the code linked, so the text is
// unlinked on the way out: SECTION words lose the base, FUNC and UND words go back to the 0 xlate wrote
enum { BG_NONE, BG_SENDING, BG_DONE };
static volatile int bg_state;
static XlPack bg_pack;
static uint32_t bg_mcrc, bg_text_crc;
static void *volatile bg_spare;  // the upload's packed relocations, for the ROM cache once the entry is sent
static uint32_t bg_spare_len;

static int src_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    (void)ctx;
    uint64_t t = timer_us_gettime64();
    int ok = cart_read(off, dst, len);
    rd_us += timer_us_gettime64() - t, rd_calls++, rd_bytes += len;
    return ok;
}

void reader_take_reads(uint32_t *calls, uint32_t *bytes, uint64_t *us) {
    *calls = rd_calls, *bytes = rd_bytes, *us = rd_us;
    rd_calls = rd_bytes = 0, rd_us = 0;
}

static int src_tables(void *ctx, uint32_t **starts, uint32_t *ns, uint32_t **ptrs, uint32_t *np, uint32_t *lo,
                      uint32_t *hi) {
    (void)ctx;
    uint32_t w[MI_N];
    if (!cart_rom_is_z64() || !cart_sync(CART_SYNC_PASS, 0, 0)) return 0;
    uint64_t t0 = timer_ms_gettime64();
    do {
        thd_sleep(20);
        if (!cart_media_info(w, MI_N)) return 0;
    } while (w[N64_MI_PASS] != PASS_DONE && w[N64_MI_PASS] != PASS_FAILED && timer_ms_gettime64() - t0 < 30000);
    if (w[N64_MI_PASS] != PASS_DONE) {
        printf("cartboot: the reader's ROM pass %s\n", w[N64_MI_PASS] == PASS_FAILED ? "overflowed" : "timed out");
        return 0;
    }
    printf("cartboot: reader's ROM pass %lu ms, %lu starts\n", (unsigned long)(timer_ms_gettime64() - t0),
           (unsigned long)w[N64_MI_STARTS]);
    uint32_t *bits = malloc(PASS_BITMAP);
    *ns = w[N64_MI_STARTS];
    *starts = malloc(4 * *ns + 4);
    if (!bits || !*starts || !cart_space_read(CART_SPACE_TABLES, 0, bits, PASS_BITMAP) ||
        !cart_space_read(CART_SPACE_TABLES, PASS_STARTS_OFF, *starts, 4 * *ns)) {
        free(bits), free(*starts);
        return 0;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < PASS_BITMAP / 4; i++)
        for (uint32_t b = bits[i]; b; b &= b - 1) n++;
    *ptrs = malloc(4 * n + 4), *np = 0;
    for (uint32_t i = 0; i < PASS_BITMAP / 4; i++)
        for (uint32_t k = 0; k < 32; k++)
            if (bits[i] >> k & 1) (*ptrs)[(*np)++] = PASS_PTR_LO + 4 * (32 * i + k);
    free(bits);
    *lo = PASS_PTR_LO, *hi = PASS_PTR_HI;
    return 1;
}

static uint32_t src_cache_get(void *ctx, uint8_t **blob) {
    (void)ctx;
    uint32_t w[MI_N];
    if (!cart_media_info(w, MI_N) || !w[N64_MI_CACHE] || (w[N64_MI_CACHE] & 3)) return 0;
    *blob = malloc(w[N64_MI_CACHE]);
    if (!*blob || !cart_space_read(CART_SPACE_CACHE, 0, *blob, w[N64_MI_CACHE])) return 0;
    return w[N64_MI_CACHE];
}

static int src_cache_put(void *ctx, const uint8_t *blob, uint32_t len) {
    (void)ctx;
    if (!cart_space_write(CART_SPACE_CACHE, 0, blob, len)) {
        printf("cartboot: the reader refused the cache blob (%lu bytes)\n", (unsigned long)len);
        return 0;
    }
    if (!cart_sync(CART_SYNC_CACHE_COMMIT, len, cart_crc32(blob, len))) {
        printf("cartboot: the reader refused to commit the cache\n");
        return 0;
    }
    return 1;
}

static int commit_busy(uint32_t *w) { return cart_media_info(w, MI_N) && w[N64_MI_COMMIT] == COMMIT_BUSY; }

int reader_commit_wait(void (*idle)(void)) {
    uint32_t w[MI_N];
    uint64_t t0 = timer_ms_gettime64();
    while (commit_busy(w) && timer_ms_gettime64() - t0 < 10000) {
        if (idle) idle();
        thd_sleep(20);
    }
    return w[N64_MI_COMMIT] == COMMIT_DONE;
}

// The xcache state once it leaves BUSY, else BUSY after ms; NONE from firmware without it
static uint32_t xc_wait(uint32_t *w, uint32_t ms) {
    uint64_t t0 = timer_ms_gettime64();
    for (;;) {
        if (!cart_media_info(w, MI_XN)) return XC_NONE;
        if (w[N64_MI_XC] != XC_BUSY || timer_ms_gettime64() - t0 > ms) return w[N64_MI_XC];
        thd_sleep(2);
    }
}

// Once the reader is idle, a lookup of key (a, b): the state it ends in; NONE / BUSY when it can't take one, NOLINK
// when the lookup didn't get to it
#define XC_NOLINK 0xFFu
static uint32_t xc_lookup(uint32_t *w, uint32_t a, uint32_t b) {
    uint32_t s = xc_wait(w, 5000);  // a store may still be going to the card
    if (s == XC_NONE || s == XC_BUSY) return s;
    return cart_sync(CART_SYNC_XC_LOOKUP, a, b) ? xc_wait(w, 1000) : XC_NOLINK;
}

// The looked-up entry's total bytes, XC_STEP per read: into dst + offset, or through xc_buf to feed
static int xc_read(uint32_t total, uint8_t *dst, int (*feed)(const uint8_t *b, uint32_t n)) {
    for (uint32_t at = 0; at < total; at += XC_STEP) {
        uint32_t n = total - at < XC_STEP ? total - at : XC_STEP;
        uint8_t *b = dst ? dst + at : xc_buf;
        if (!cart_space_read(CART_SPACE_XCACHE, at, b, n) || (feed && !feed(b, n))) return 0;
    }
    return 1;
}

// An entry's bytes at offset at, step bytes per write so other link users get their turn between them
static int xc_write(uint32_t at, const uint8_t *src, uint32_t len, uint32_t step) {
    for (uint32_t i = 0; i < len; i += step) {
        if (!cart_space_write(CART_SPACE_XCACHE, at + i, src + i, len - i < step ? len - i : step)) return 0;
        thd_pass();
    }
    return 1;
}

// p's stored form (xlpack.h) to the reader and committed. linked: p's text after tload, unlinked on the way and
// checked against text_crc, its CRC before tload
static int xc_store(const XlPack *p, const uint8_t *linked, uint32_t text_crc, uint32_t step, uint32_t mcrc) {
    uint32_t w[MI_XN], at = 0, crc = 0, tcrc = 0;
    uint64_t t0 = timer_ms_gettime64(), us0 = timer_us_gettime64();
    while (commit_busy(w) && timer_ms_gettime64() - t0 < 10000)
        thd_sleep(20);  // the flash commit holds the QSPI bus the reader stages entries over
    uint32_t s = xc_wait(w, 5000);
    s = s != XC_NONE && cart_sync(CART_SYNC_XC_RESERVE, xlp_store_max(p), 0) ? xc_wait(w, 1000) : XC_FAILED;
    if (s == XC_FULL) printf("xcache: %lu KB won't fit on the reader's SD\n", (unsigned long)(xlp_store_max(p) >> 10));
    int ok = s == XC_RECV;
    for (uint32_t k = 0, n; ok && (n = xlp_record(p, k, linked, xc_rec, xc_scratch, xc_work, &tcrc)); k++) {
        crc = xl_crc32(crc, xc_rec, n);
        ok = xc_write(at, xc_rec, n, step);
        at += n;
    }
    if (ok && linked && tcrc != text_crc) {
        printf("xcache: the unlinked code's CRC %08lX isn't xlate's %08lX; not stored\n", (unsigned long)tcrc,
               (unsigned long)text_crc);
        ok = 0, s = XC_FAILED;
    }
    ok = ok && cart_sync(CART_SYNC_XC_COMMIT, at, crc);
    uint32_t ms = (uint32_t)((timer_us_gettime64() - us0) / 1000);
    if (ok)
        printf("xcache: %lu KB (%lu KB unpacked) sent to the reader in %lu ms (%lu KB/s)%s; it writes the SD on its own\n",
               (unsigned long)(at >> 10), (unsigned long)(p->total >> 10), (unsigned long)ms,
               (unsigned long)((at >> 10) * 1000 / (ms + 1)), linked ? " during play" : "");
    else if (s != XC_FULL)
        printf("xcache: meta %08lX not stored (reader state %lu)\n", (unsigned long)mcrc, (unsigned long)s);
    return ok;
}

static void *bg_upload(void *arg) {
    (void)arg;
    xc_store(&bg_pack, g_boot_out->text, bg_text_crc, BG_STEP, bg_mcrc);
    xl_free(bg_pack.names);
    bg_spare_len = bg_pack.sec[1].len + 4;
    bg_spare = bg_pack.rel;
    bg_state = BG_DONE;
    return NULL;
}

void reader_upload_start(uint32_t text_crc) {
    if (bg_state != BG_SENDING) return;
    bg_text_crc = text_crc;
    if (!thd_create(1, bg_upload, NULL)) {
        printf("xcache: no thread for the upload; not stored\n");
        xl_pack_free(&bg_pack);
        bg_state = BG_DONE;
    }
}

int reader_upload_spare(void **mem, uint32_t *len) {
    if (!bg_spare) return 0;
    *mem = bg_spare, *len = bg_spare_len;
    bg_spare = NULL;
    return 1;
}

static int load_feed(const uint8_t *b, uint32_t n) {
    reader_xc_done += n;
    return xlp_load_feed(&xc_load, b, n);
}

static int src_xc_get(void *ctx, uint32_t mcrc, uint32_t mlen, XlOut *o) {
    (void)ctx;
    if (bg_state == BG_SENDING) return 0;  // the reader holds one entry at a time: an overlay translates meanwhile
    uint32_t w[MI_XN];
    uint64_t t0 = timer_us_gettime64();
    reader_xc_done = reader_xc_total = 0;
    uint32_t s = xc_lookup(w, mcrc, xl_pack_key(mlen));
    if (s == XC_NOLINK) return 0;
    if (s == XC_NONE || s == XC_BUSY) {
        if (!xc_quiet++) printf("xcache: %s\n", s == XC_NONE ? "the reader has no SD cache" : "the reader stays busy");
        return 0;
    }
    if (s != XC_HIT) {
        printf("xcache: meta %08lX %s\n", (unsigned long)mcrc, s == XC_MISS ? "not on the reader's SD" : "lookup failed");
        return 0;
    }
    uint32_t total = w[N64_MI_XC_BYTES];
    reader_xc_total = total;
    xlp_load_begin(&xc_load, mcrc, mlen, o);
    int ok = xc_read(total, NULL, load_feed);
    ok = ok && xlp_load_end(&xc_load) && xc_wait(w, 1000) == XC_HIT;  // the reader checks the card's CRC as it loads
    if (!ok) {
        xlp_load_drop(&xc_load);
        printf("xcache: the reader's copy of meta %08lX didn't read back; translating\n", (unsigned long)mcrc);
        return 0;
    }
    uint32_t ms = (uint32_t)((timer_us_gettime64() - t0) / 1000), kb = total >> 10;
    printf("xcache: %lu KB (%lu KB unpacked) from the reader's SD in %lu ms (%lu KB/s)\n", (unsigned long)kb,
           (unsigned long)(xl_unpack_total(xc_load.in) >> 10), (unsigned long)ms, (unsigned long)(kb * 1000 / (ms + 1)));
    return 1;
}

static int src_xc_put(void *ctx, uint32_t mcrc, uint32_t mlen, const XlOut *o) {
    (void)ctx;
    if (bg_state == BG_SENDING) return 0;
    if (o == g_boot_out) {  // packed now, sent by bg_upload once the game runs
        reader_xc_done = reader_xc_total = 0;
        if (!xl_pack(o, mcrc, mlen, &bg_pack)) {
            xl_pack_free(&bg_pack);
            return 0;
        }
        bg_mcrc = mcrc, bg_state = BG_SENDING;
        return 1;
    }
    XlPack p;
    int ok = xl_pack(o, mcrc, mlen, &p) && xc_store(&p, NULL, 0, XC_STEP, mcrc);
    xl_pack_free(&p);
    return ok;
}

// A run-time code region's scan (harness/rgn.c): one blob, stored as it is, in the same SD cache under its own key
static uint32_t src_xb_get(void *ctx, uint32_t a, uint32_t b, uint8_t **blob) {
    (void)ctx;
    if (bg_state == BG_SENDING) return 0;
    uint32_t w[MI_XN];
    if (xc_lookup(w, a, b) != XC_HIT) return 0;
    uint32_t total = w[N64_MI_XC_BYTES];
    uint8_t *p = malloc(total + 4);
    if (!p || !xc_read(total, p, NULL) || xc_wait(w, 1000) != XC_HIT) {  // the reader checks the card's CRC as it loads
        free(p);
        return 0;
    }
    *blob = p;
    return total;
}

static int src_xb_put(void *ctx, uint32_t a, uint32_t b, const uint8_t *blob, uint32_t len) {
    (void)ctx;
    if (bg_state == BG_SENDING || (len & 3)) return 0;
    uint32_t w[MI_XN], s = xc_lookup(w, a, b);  // the store takes the lookup's key
    if (s == XC_NONE || s == XC_BUSY || s == XC_NOLINK) return 0;
    if (s == XC_HIT) return 1;
    s = s == XC_MISS && cart_sync(CART_SYNC_XC_RESERVE, len, 0) ? xc_wait(w, 1000) : XC_FAILED;
    int ok = s == XC_RECV && xc_write(0, blob, len, XC_STEP) && cart_sync(CART_SYNC_XC_COMMIT, len, xl_crc32(0, blob, len));
    printf("xcache: region scan %08lX %s (%lu KB)\n", (unsigned long)a, ok ? "sent to the reader" : "not stored",
           (unsigned long)(len >> 10));
    return ok;
}

void reader_src(CartSrc *src, uint32_t rom_size, const XlOut *boot_out) {
    g_boot_out = boot_out;
    *src = (CartSrc){ .size = rom_size, .read = src_read, .tables = src_tables, .cache_get = src_cache_get,
                      .cache_put = src_cache_put, .xc_get = src_xc_get, .xc_put = src_xc_put, .xb_get = src_xb_get,
                      .xb_put = src_xb_put };
}
