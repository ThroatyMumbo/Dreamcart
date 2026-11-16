#include "cartboot.h"

#include <string.h>

#include "boot.h"
#include "cartinfo.h"
#include "ovl.h"
#include "rgn.h"
#include "tload.h"

#define CHUNK 0x10000u   // stream reads
#define KEEP 0x110000u   // the header segment's ROM stays in pages from the stream: the scan reads most of it

struct cart_info cart_info;

typedef struct {
    const CartSrc *src;
    uint32_t **pages, npages;
    const char *err;
} Pages;

static void to_words(uint32_t *w, const uint8_t *b, uint32_t nwords) {
    for (uint32_t i = 0; i < nwords; i++, b += 4) w[i] = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

int cb_read_page(int (*read)(void *ctx, uint32_t off, void *dst, uint32_t len), void *ctx, uint32_t size, uint32_t pg,
                 uint32_t *w) {
    uint8_t b[4096];
    uint32_t off = 4096 * pg;
    memset(b, 0, sizeof b);
    if (off < size && !read(ctx, off, b, size - off < 4096 ? size - off : 4096)) return 0;
    to_words(w, b, 1024);
    return 1;
}

static const uint32_t *page(void *ctx, uint32_t pg) {
    Pages *p = ctx;
    if (pg >= p->npages) cs_die("cartboot: page past the ROM");
    if (!p->pages[pg]) {
        uint32_t *w = cs_alloc(4096);
        if (!cb_read_page(p->src->read, p->src->ctx, p->src->size, pg, w)) cs_die("cartboot: cart read failed");
        p->pages[pg] = w;
    }
    return p->pages[pg];
}

static void *alloc(size_t n) { return cs_alloc(n); }

// Every ROM word through cs_feed, keeping the header segment's pages on the way
static const char *stream(const CartSrc *src, Scan *s, Pages *P) {
    uint8_t *buf = cs_alloc(CHUNK);
    uint32_t *words = cs_alloc(CHUNK);
    for (uint32_t off = 0; off < src->size; off += CHUNK) {
        uint32_t n = src->size - off < CHUNK ? src->size - off : CHUNK;
        if (!src->read(src->ctx, off, buf, n)) return "cartboot: cart read failed";
        to_words(words, buf, n / 4);
        cs_feed(s, words, n / 4);
        for (uint32_t k = 0; off + 4096 * k < KEEP && 4096 * k < n; k++) {
            uint32_t pg = off / 4096 + k;
            if (!P->pages[pg] && 4096 * (k + 1) <= n) {
                P->pages[pg] = cs_alloc(4096);
                memcpy(P->pages[pg], words + 1024 * k, 4096);
            }
        }
    }
    cs_release(buf), cs_release(words);
    return NULL;
}

static void phase(const char *name, uint64_t t0, uint64_t (*now_us)(void)) {
    cs_log("cartboot: %s done at %lu ms, heap %lu KB\n", name, (unsigned long)((now_us() - t0) / 1000),
           (unsigned long)(cartboot_heap_used() >> 10));
}

static void stage(int s) { cartboot_stage(s); }

static int has_overlays(const CsResult *r) {
    for (uint32_t i = 0; i < r->nseg; i++)
        if (meta_seg_is_ovl(r, i)) return 1;
    return 0;
}

// The MIPS instructions boot translates: every function, or only the fixed segments' when overlays wait for their load
static uint32_t boot_mips(const CsResult *r, int lazy) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->nfunc; i++)
        if (!lazy || meta_seg_holds(r, -1, r->func[i].start)) n += (r->func[i].end - r->func[i].start) / 4;
    return n;
}

// Per function, whether it takes every register: each segment's own XLM3 through xlate's pre-pass, one at a time
uint8_t *cb_raw_bits(const CsResult *r, const CbHooks *hk, cs_page_fn page, void *ctx, uint32_t size) {
    uint8_t *raw = cs_alloc(r->nfunc + 1), *part = cs_alloc(r->nfunc + 1);
    memset(raw, 0, r->nfunc + 1);
    for (int seg = -1; seg < (int)r->nseg; seg++) {
        if (seg >= 0 && !meta_seg_is_ovl(r, (uint32_t)seg)) continue;
        uint32_t len, n = 0;
        uint8_t *m = meta_write_seg(r, hk, NULL, seg, page, ctx, size, &len);
        xl_raw_pass(m, len, part);
        cs_release(m);
        for (uint32_t i = 0; i < r->nfunc; i++)
            if (meta_seg_holds(r, seg, r->func[i].start)) raw[i] = part[n++];
    }
    cs_release(part);
    return raw;
}

static void pages_free(Pages *P) {
    for (uint32_t i = 0; i < P->npages; i++) cs_release(P->pages[i]);
    cs_release(P->pages);
}

// The scan, from the adapter's tables first and the whole ROM if they fall short; NULL on a read failure
static Scan *scan_cart(const CartSrc *src, Pages *P, CartBootStats *st, uint64_t (*now_us)(void), uint64_t t0, uint64_t *t,
                       const CsResult **r, const char **err) {
    for (int whole = 0;; whole = 1) {
        stage(CB_STAGE_TABLES);
        Scan *s = cs_new(src->size, page, P);
        uint32_t *starts, nstarts, *ptrs, nptrs, lo, hi;
        st->tables = !whole && src->tables && src->tables(src->ctx, &starts, &nstarts, &ptrs, &nptrs, &lo, &hi);
        if (st->tables) {
            cs_set_tables(s, starts, nstarts, ptrs, nptrs, lo, hi);
            cs_release(starts), cs_release(ptrs);
        } else if ((*err = stream(src, s, P))) {
            return NULL;
        }
        st->us_stream += now_us() - *t, *t = now_us();
        phase(st->tables ? "adapter tables" : "stream", t0, now_us);
        stage(CB_STAGE_SCAN);
        *r = cs_run(s);
        if (!(*r)->tables_short) return s;
        cs_log("cartboot: a segment lies outside the adapter's pointer range; scanning the whole ROM\n");
        cs_free(s);
    }
}

// No cache for this cart: the scan and the signatures build its blob, stored back to the reader, and give back every
// page; the boot then starts again from the blob
static const char *boot_scan(const CartSrc *src, Pages *P, const uint32_t *header, CartBootStats *st,
                             uint64_t (*now_us)(void), uint64_t t0, uint64_t *t, uint8_t **out, uint32_t *out_len) {
    const char *err = NULL;
    const CsResult *r;
    Scan *s = scan_cart(src, P, st, now_us, t0, t, &r, &err);
    if (!s) return err;
    cs_drop_work(s);
    st->nfunc = r->nfunc;
    int lazy = has_overlays(r);
    st->mips = boot_mips(r, lazy);
    st->us_scan = now_us() - *t, *t = now_us();
    phase("scan", t0, now_us);

    stage(CB_STAGE_SIGS);
    HmResult *h = hm_run(r, page, P, src->size);
    if (h->os_missing) return "cartboot: libultra's thread queue, event table or RCP mask table not found";
    cart_info.thread_tail = h->os_thread_tail, cart_info.event_tab = h->os_event_tab;
    cart_info.rcp_im_table = h->os_rcp_im_table, cart_info.destroy_thread = h->os_destroy_thread;
    st->us_sigs = now_us() - *t, *t = now_us();
    phase("signatures", t0, now_us);

    stage(CB_STAGE_META);
    uint8_t *raw = NULL;
    if (lazy) {
        CbHooks hk;
        hooks_from_hm(h, &hk);
        raw = cb_raw_bits(r, &hk, page, P, src->size);
        xl_release();  // its last raw pass's meta would sit in the middle of what the restart frees
        cs_release(hk.addr), cs_release(hk.pol);
    }
    uint32_t clen;
    uint8_t *c = cache_build(r, h, raw, src->size, header, &clen);
    if (src->cache_put) {
        stage(CB_STAGE_STORE);
        st->cache_stored = src->cache_put(src->ctx, c, clen) ? clen : 0;
    }
    // all the scan given back before the copy: anything built before the free splits xlate's text (SM64 US)
    cs_release(raw), hm_free(h), cs_free(s);
    pages_free(P);
    *out = cs_alloc(clen);
    memcpy(*out, c, clen);
    *out_len = clen;
    cs_release(c);
    return NULL;
}

// The cache's scan and signature results back, as the meta xlate takes; lazy when overlays wait for their load
static uint8_t *cached_meta(const CartSrc *src, Pages *P, CbCache *cache, CartBootStats *st, int *lazy) {
    stage(CB_STAGE_META);
    cart_info.thread_tail = cache->os[0], cart_info.event_tab = cache->os[1];
    cart_info.rcp_im_table = cache->os[2], cart_info.destroy_thread = cache->os[3];
    st->nfunc = cache->r.nfunc;
    *lazy = has_overlays(&cache->r);
    st->mips = boot_mips(&cache->r, *lazy);
    if (!*lazy) return meta_write(&cache->r, &cache->hk, page, P, src->size, &st->meta_len);
    ovl_reserve(&cache->r);
    if (!cache->raw) cache->raw = cb_raw_bits(&cache->r, &cache->hk, page, P, src->size);
    return meta_write_seg(&cache->r, &cache->hk, cache->raw, -1, page, P, src->size, &st->meta_len);
}

static const char *xlate_and_link(const CartSrc *src, XlOut *xo, CartBootStats *st, void (*sync)(void *p, size_t n),
                                  uint64_t (*now_us)(void), uint64_t t0, uint64_t t, uint8_t *meta, CbCache *ctx, int lazy);

// given: a cache blob this boot owns, from the first boot that just built it; else the reader's, if any. A cart with
// no cache returns from its scan with *built set, for cartboot to boot again from
static const char *boot(const CartSrc *src, uint32_t *rdram, XlOut *xo, CartBootStats *st, void (*sync)(void *p, size_t n),
                        uint64_t (*now_us)(void), uint8_t *given, uint32_t given_len, uint8_t **built, uint32_t *built_len) {
    memset(st, 0, sizeof *st);
    if (src->size < 0x101000u) return "cartboot: ROM smaller than its boot segment";
    Pages P = {src, NULL, (src->size + 4095) / 4096, NULL};
    P.pages = cs_alloc(P.npages * sizeof *P.pages);
    memset(P.pages, 0, P.npages * sizeof *P.pages);

    uint64_t t = now_us(), t0 = t;
    stage(CB_STAGE_CACHE);
    const uint32_t *header = page(&P, 0);
    const char *err;
    CbCache cache;
    uint8_t *blob = given;
    uint32_t blen = given ? given_len : src->cache_get ? src->cache_get(src->ctx, &blob) : 0;
    st->cached = blen && cache_parse(blob, blen, src->size, header, &cache);
    st->cache_len = blen;
    if (given && !st->cached) return "cartboot: its own cache blob doesn't parse";
    st->us_cache = now_us() - t, t = now_us();
    if (!st->cached) return boot_scan(src, &P, header, st, now_us, t0, &t, built, built_len);
    phase("cache", t0, now_us);
    int lazy;
    uint8_t *meta = cached_meta(src, &P, &cache, st, &lazy);
    CbCache ctx = cache;  // run-time code links against the fixed code; a cart without overlays gets raw bits only then
    cs_release(blob);
    st->meta_crc = cs_crc32(0, meta, st->meta_len);

    stage(CB_STAGE_IMAGE);
    BootInfo bi;
    if ((err = bi_build(rdram, page, &P, src->size, &bi))) return err;
    cart_info.main = bi.main, cart_info.sp = bi.sp, cart_info.sp_fill = bi.sp_fill, cart_info.imem0 = bi.imem0;
    memcpy(cart_info.header, bi.header, sizeof bi.header);
    st->boot_crc = cs_crc32(0, rdram, BOOT_RDRAM);
    pages_free(&P);
    st->us_meta = now_us() - t, t = now_us();
    phase("meta and boot image", t0, now_us);
    return xlate_and_link(src, xo, st, sync, now_us, t0, t, meta, &ctx, lazy);
}

// xlate's text from the reader's SD or translated here, then linked; overlays and run-time regions set up after
static const char *xlate_and_link(const CartSrc *src, XlOut *xo, CartBootStats *st, void (*sync)(void *p, size_t n),
                                  uint64_t (*now_us)(void), uint64_t t0, uint64_t t, uint8_t *meta, CbCache *ctx, int lazy) {
    const char *err;
    XlStats xs;
    memset(&xs, 0, sizeof xs);
    memset(xo, 0, sizeof *xo);
    if (src->xc_get) stage(CB_STAGE_XC_GET);
    st->xc_hit = src->xc_get && src->xc_get(src->ctx, st->meta_crc, st->meta_len, xo);
    if (!st->xc_hit) {
        stage(CB_STAGE_XLATE);
        xo->cap = st->mips * 10 + 0x10000;  // measured 7.4 (SM64) and 9.7 (MK64) bytes per MIPS instruction
        xo->text = cs_alloc(xo->cap);
        xo->caprel = st->mips / 2 + 1024;
        xo->rel = cs_alloc(xo->caprel * sizeof *xo->rel);
        xl_run(meta, st->meta_len, 2, xo, &xs);
    }
    cs_release(meta);
    st->text = xo->len, st->nrel = xo->nrel;
    st->text_crc = cs_crc32(0, xo->text, xo->len);
    st->rel_crc = xl_reloc_crc(xo);
    st->nhooked = xs.hook_native + xs.hook_noop + xs.hook_unsupported;
    st->us_xlate = now_us() - t, t = now_us();
    phase(st->xc_hit ? "xlate, from the reader's SD" : "xlate", t0, now_us);
    if (!st->xc_hit && src->xc_put) {
        stage(CB_STAGE_XC_PUT);
        st->xc_stored = src->xc_put(src->ctx, st->meta_crc, st->meta_len, xo);
        st->us_xc_put = now_us() - t, t = now_us();
    }

    stage(CB_STAGE_LINK);
    uint8_t *stubs;
    uint32_t stub_len;
    if ((err = tload(xo, alloc, &stubs, &stub_len))) return err;
    xl_release();
    xo->alias_name = NULL, xo->nalias = 0;
    st->stubs = stub_len / TLOAD_STUB;
    sync(xo->text, xo->len);
    if (stub_len) sync(stubs, stub_len);
    cs_release(xo->rel);  // linked: only the CRC above needed it
    xo->rel = NULL, xo->caprel = 0;
    cs_log("cartboot: %lu functions, %lu jump tables and %lu hooks kept for run-time code\n", (unsigned long)ctx->r.nfunc,
           (unsigned long)ctx->r.ntab, (unsigned long)ctx->hk.n);
    if (lazy) ovl_init(ctx, src->size, sync);
    else ovl_runtime_init(ctx, src->size, sync);
    rgn_init();
    ovl_xcache(src->xc_get, src->xc_put, src->ctx);
    rgn_xcache(src->xb_get, src->xb_put, src->ctx);
    st->us_load = now_us() - t;
    return NULL;
}

const char *cartboot(const CartSrc *src, uint32_t *rdram, XlOut *xo, CartBootStats *st, void (*sync)(void *p, size_t n),
                     uint64_t (*now_us)(void)) {
    uint8_t *blob = NULL, *unused;
    uint32_t len = 0, unused_len;
    const char *err = boot(src, rdram, xo, st, sync, now_us, NULL, 0, &blob, &len);
    if (err || !blob) return err;
    CartBootStats first = *st;
    cs_log("cartboot: cache built; booting again from it\n");
    err = boot(src, rdram, xo, st, sync, now_us, blob, len, &unused, &unused_len);
    st->cached = 0, st->tables = first.tables, st->cache_stored = first.cache_stored;
    st->us_stream = first.us_stream, st->us_scan = first.us_scan, st->us_sigs = first.us_sigs;
    return err;
}
