#include "ovl.h"

#include <string.h>

#include "cartboot.h"
#include "os.h"
#include "rcp.h"
#include "tload.h"
#include "tmpl.h"
#include "view.h"
#include "xlate.h"

#define OV_MAX CS_OV_ROM_MAX
#define TEXT_PER_INS 11u  // SH-4 bytes per MIPS instruction: 7.4 to 10.5 measured (Kirby 64's ovl 8 the most)
#define RELS(ins) ((ins) * 2 / 3 + 1024)  // relocations for ins MIPS instructions: 0.62 per instruction the most measured

typedef struct {
    uint32_t seg, vram, size, ins, nfunc;
    uint32_t off, len;  // its arena block; len 0 while untranslated
    uint32_t nkey, *keys, stamp;
} Ov;

typedef struct { uint32_t off, len, who; } Blk;

static struct {
    CbCache c;
    uint32_t rom_size;
    void (*sync)(void *p, size_t n);
    Ov ov[OV_MAX];
    uint32_t nov;
    uint8_t *arena;
    uint32_t arena_size, work;
    Blk blk[OV_MAX];
    uint32_t nblk, clock;
    uint32_t pg_no, pg_ok, pg[1024];
} V;

static uint8_t resident[OV_MAX];  // by ovtab entry, which is scan order
static int needing;  // ovl_init has checked the ovtab against the reserve

static int cart_read(void *ctx, uint32_t off, void *dst, uint32_t len) {
    (void)ctx;
    return rcp_cart_read(off, dst, len);
}
int ovl_read_page(uint32_t pg, uint32_t *w) { return cb_read_page(cart_read, NULL, V.rom_size, pg, w); }

const uint32_t *ovl_page(void *ctx, uint32_t pg) {
    (void)ctx;
    if (V.pg_ok && V.pg_no == pg) return V.pg;
    uint32_t off = 4096 * pg;
    if (off >= V.rom_size || !ovl_read_page(pg, V.pg)) os_die("ovl: cart read failed at ROM offset", off);
    V.pg_no = pg, V.pg_ok = 1;
    return V.pg;
}

void ovl_arena_drop(uint32_t who) {
    for (uint32_t k = 0; k < V.nblk; k++)
        if (V.blk[k].who == who) {
            memmove(&V.blk[k], &V.blk[k + 1], (V.nblk - k - 1) * sizeof *V.blk);
            V.nblk--;
            return;
        }
}

static void evict(uint32_t j) {
    Ov *o = &V.ov[j];
    for (uint32_t k = 0; k < o->nkey; k++) t_lookup_remove(o->keys[k]);
    cs_release(o->keys);
    o->keys = NULL, o->nkey = 0, o->len = 0;
    ovl_arena_drop(j);
}

// First fit in the arena; the least recently needed translation that isn't resident goes until it fits
static uint32_t arena_get(uint32_t n, uint32_t who) {
    n = (n + 31) & ~31u;
    for (;;) {
        uint32_t at = 0;
        for (uint32_t k = 0; k <= V.nblk; k++) {
            uint32_t end = k < V.nblk ? V.blk[k].off : V.arena_size;
            if (end >= at && end - at >= n) {
                memmove(&V.blk[k + 1], &V.blk[k], (V.nblk - k) * sizeof *V.blk);
                V.blk[k] = (Blk){at, n, who};
                V.nblk++;
                return at;
            }
            if (k < V.nblk) at = V.blk[k].off + V.blk[k].len;
        }
        int victim = -1;
        for (uint32_t j = 0; j < V.nov; j++)
            if (V.ov[j].len && j != who && !resident[j] && (victim < 0 || V.ov[j].stamp < V.ov[victim].stamp))
                victim = (int)j;
        if (victim < 0) os_die("ovl: no room in the overlay arena for bytes", n);
        evict((uint32_t)victim);
    }
}

static struct {
    int (*get)(void *ctx, uint32_t meta_crc, uint32_t meta_len, XlOut *o);
    int (*put)(void *ctx, uint32_t meta_crc, uint32_t meta_len, const XlOut *o);
    void *ctx;
} X;

void ovl_xcache(int (*get)(void *ctx, uint32_t meta_crc, uint32_t meta_len, XlOut *o),
                int (*put)(void *ctx, uint32_t meta_crc, uint32_t meta_len, const XlOut *o), void *ctx) {
    X.get = get, X.put = put, X.ctx = ctx;
}

static uint8_t *place_arena(uint32_t n, uint32_t who, uint32_t *off) {
    *off = arena_get(n, who);
    return V.arena + *off;
}

// Never freed: host return addresses may point into it
static uint8_t *place_heap(uint32_t n, uint32_t who, uint32_t *off) {
    (void)who;
    uint8_t *p = cs_alloc(n + 31);
    if (!p) os_die("ovl: no heap for translated bytes", n);
    *off = 0;
    return (uint8_t *)(((uintptr_t)p + 31) & ~(uintptr_t)31);  // the arena's alignment, so code lays out the same
}

uint8_t *ovl_place_ram(uint32_t n, uint32_t who, uint32_t *off) {
    return V.arena ? place_arena(n, who, off) : place_heap(n, who, off);
}

// xlate's output for one segment's meta (the reader's copy when cached says it may be), placed where place puts it
// and linked there, names outside it resolved by lookup; its function keys go in the lookup
uint8_t *ovl_link_meta(const uint8_t *meta, uint32_t mlen, uint32_t ins, int cached, uint32_t who, uint32_t vram,
                       OvlPlace place, uint32_t (*lookup)(uint32_t), uint32_t *off, uint32_t *len, uint32_t **keys,
                       uint32_t *nkey, XlStats *xs) {
    XlOut xo;
    memset(&xo, 0, sizeof xo);
    memset(xs, 0, sizeof *xs);
    uint32_t mcrc = cached && (X.get || X.put) ? cs_crc32(0, meta, mlen) : 0;
    if (!cached || !X.get || !X.get(X.ctx, mcrc, mlen, &xo)) {
        xo.cap = ins * TEXT_PER_INS + 0x10000;
        xo.text = cs_alloc(xo.cap);
        xo.caprel = RELS(ins);
        xo.rel = cs_alloc(xo.caprel * sizeof *xo.rel);
        xl_run(meta, mlen, 2, &xo, xs);
        if (cached && X.put) X.put(X.ctx, mcrc, mlen, &xo);
    }
    uint32_t *und = cs_alloc(xo.nund * 4 + 4), bytes = tload_blob_size(&xo, lookup, und);
    if (!bytes) os_die("ovl: an overlay names a symbol the runtime lacks", vram);
    uint8_t *base = place(bytes, who, off);
    const char *err = tload_blob(&xo, base, lookup, und);
    if (err) os_die(err, vram);
    V.sync(base, bytes);
    const uint32_t *ft = (const uint32_t *)(base + xo.functab_off);
    *nkey = *(const uint32_t *)(base + xo.functab_count_off);
    *keys = cs_alloc(*nkey * 4 + 4);
    for (uint32_t k = 0; k < *nkey; k++) (*keys)[k] = ft[2 * k], t_lookup_insert(ft[2 * k], ft[2 * k + 1]);
    *len = bytes;
    cs_release(und);
    xl_free_out(&xo);
    xl_release();
    return base;
}

static void translate(uint32_t i) {
    Ov *o = &V.ov[i];
    uint64_t t0 = xl_now_us();
    uint32_t mlen;
    uint8_t *meta = meta_write_seg(&V.c.r, &V.c.hk, V.c.raw, (int)o->seg, ovl_page, NULL, V.rom_size, &mlen);
    XlStats xs;
    uint8_t *base = ovl_link_meta(meta, mlen, o->ins, 1, i, o->vram, place_arena, t_lookup_exact, &o->off, &o->len,
                                  &o->keys, &o->nkey, &xs);
    cs_log("ovl: %08lx rom %06lx, %lu instructions %sin %lu ms, %lu KB of arena %lu KB, code at %08lx\n",
           (unsigned long)o->vram, (unsigned long)V.c.r.seg[o->seg].rom, (unsigned long)o->ins,
           xs.mips ? "" : "from the reader's SD ",
           (unsigned long)((xl_now_us() - t0) / 1000), (unsigned long)(o->len >> 10), (unsigned long)(V.arena_size >> 10),
           (unsigned long)(uintptr_t)base);
    cs_release(meta);
}

static void need(uint32_t i) {
    if (i >= V.nov) return;
    V.ov[i].stamp = ++V.clock;
    if (!V.ov[i].len) os_big_call(translate, i);
}

void ov_dma(uint32_t rom_off, uint32_t dram, uint32_t len) {
    uint32_t a = 0x80000000u | dram;
    for (uint32_t i = 0; i < t_ovtab_n && i < OV_MAX; i++) {
        const uint32_t *e = t_ovtab_p + 4 * i;
        if (!(a < e[0] + e[1] && e[0] < a + len)) continue;
        int now = rom_off - e[2] == a - e[0];
        resident[i] = (uint8_t)now;
        if (now && needing) need(i);
    }
}

uint32_t ovl_key(uint32_t vram) {
    int n = 0, one = -1, res = -1;
    for (uint32_t i = 0; i < t_ovtab_n && i < OV_MAX; i++) {
        const uint32_t *e = t_ovtab_p + 4 * i;
        if (vram - e[0] >= e[1]) continue;
        n++, one = (int)i;
        if (resident[i] && res < 0) res = (int)i;
    }
    if (!n) return vram;
    if (n > 1 && res < 0) os_die("overlay: none loaded at", vram);
    return (t_ovtab_p[4 * (n > 1 ? res : one) + 3] & ~CS_OV_MASK) | (vram & CS_OV_MASK);
}

uint32_t ovl_need_key(uint32_t key) {
    if (!needing) return 0;
    for (uint32_t i = 0; i < t_ovtab_n && i < OV_MAX; i++)
        if ((key & ~CS_OV_MASK) == (t_ovtab_p[4 * i + 3] & ~CS_OV_MASK)) {
            need(i);
            return t_lookup_exact(key);
        }
    return 0;
}

// The most code the overlays can have resident at once: weighted interval scheduling over their vram ranges
static uint32_t coresident(void) {
    uint32_t order[OV_MAX], best[OV_MAX + 1];
    for (uint32_t i = 0; i < V.nov; i++) order[i] = i;
    for (uint32_t i = 1; i < V.nov; i++)
        for (uint32_t j = i; j > 0 && V.ov[order[j]].vram + V.ov[order[j]].size <
                                         V.ov[order[j - 1]].vram + V.ov[order[j - 1]].size; j--) {
            uint32_t t = order[j];
            order[j] = order[j - 1], order[j - 1] = t;
        }
    best[0] = 0;
    for (uint32_t i = 0; i < V.nov; i++) {
        const Ov *o = &V.ov[order[i]];
        uint32_t w = o->ins * TEXT_PER_INS + o->nfunc * 8 + 1024 + 32, p = 0;
        while (p < i && V.ov[order[p]].vram + V.ov[order[p]].size <= o->vram) p++;
        best[i + 1] = best[i] > best[p] + w ? best[i] : best[p] + w;
    }
    return best[V.nov];
}

// The overlays in scan order, which is the fixed meta's and so the ovtab's
void ovl_reserve(const CsResult *r) {
    uint32_t biggest = 0;
    V.nov = 0;
    for (uint32_t s = 0; s < r->nseg && V.nov < OV_MAX; s++) {
        if (!meta_seg_is_ovl(r, s)) continue;
        const CsSeg *g = &r->seg[s];
        Ov *o = &V.ov[V.nov++];
        memset(o, 0, sizeof *o);
        o->seg = s, o->vram = 0x80000000u | (g->vram & 0x7FFFFFu), o->size = g->size;
        for (uint32_t f = 0; f < r->nfunc; f++)
            if (r->func[f].start - g->vram < g->size) o->ins += (r->func[f].end - r->func[f].start) / 4, o->nfunc++;
        uint32_t w = g->size + 8 * r->nfunc + o->ins * TEXT_PER_INS + 0x10000 + RELS(o->ins) * (uint32_t)sizeof(XlReloc);
        if (w > biggest) biggest = w;
    }
    V.work = biggest;
    V.arena_size = coresident() + 0x10000;
    V.arena = cs_alloc(V.arena_size);
    cs_log("ovl: %lu overlays, arena %lu KB, %lu KB more while one translates\n", (unsigned long)V.nov,
           (unsigned long)(V.arena_size >> 10), (unsigned long)(V.work >> 10));
}

void ovl_runtime_init(CbCache *c, uint32_t rom_size, void (*sync)(void *p, size_t n)) {
    V.c = *c, V.rom_size = rom_size, V.sync = sync;
    memset(c, 0, sizeof *c);
}

void ovl_init(CbCache *c, uint32_t rom_size, void (*sync)(void *p, size_t n)) {
    ovl_runtime_init(c, rom_size, sync);
    if (t_ovtab_n != V.nov) os_die("ovl: the ovtab and the reserved overlays differ in count", t_ovtab_n);
    for (uint32_t i = 0; i < V.nov; i++)
        if (t_ovtab_p[4 * i + 3] != V.c.r.seg[V.ov[i].seg].vram) os_die("ovl: ovtab entry out of scan order", i);
    needing = 1;
}

CbCache *ovl_ctx(void) { return &V.c; }
uint32_t ovl_rom_size(void) { return V.rom_size; }
uint8_t *ovl_place_heap(uint32_t n, uint32_t who, uint32_t *off) { return place_heap(n, who, off); }

uint32_t ovl_guest_fn(uint32_t h) {
    uint32_t a = h - (uint32_t)(uintptr_t)V.arena;
    if ((h & 1) || a >= V.arena_size) return 0;
    for (uint32_t j = 0; j < V.nov; j++) {
        const Ov *o = &V.ov[j];
        if (!o->len || a - o->off >= o->len) continue;
        uint32_t best = 0, at = 0;
        for (uint32_t k = 0; k < o->nkey; k++) {
            uint32_t f = t_lookup_exact(o->keys[k]);
            if (f <= h && f > at) at = f, best = o->keys[k];
        }
        return best;
    }
    return 0;
}

uint32_t ovl_arena_bytes(void) { return V.arena_size; }
uint32_t ovl_work_bytes(void) { return V.work; }
