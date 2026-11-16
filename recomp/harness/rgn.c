#include "rgn.h"

#include <stdio.h>
#include <string.h>

#include "boot.h"
#include "cartboot.h"
#include "os.h"
#include "ovl.h"
#include "rcp.h"
#include "sigs.h"
#include "tmpl.h"
#include "view.h"
#include "xlate.h"
#include "xlpack.h"

#define RGN_MIN 0x4000u        // a region's least, and an invalidation that stales every run-time function
#define CHUNK_INS 16384u       // MIPS instructions per chunk, about 180 KB of SH-4
#define DIR_MAGIC 0x31524744u  // "DGR1"
#define WHO 0x10000u           // past the overlays' and the one-function path's arena ids
#define ERET 0x42000018u

enum { R_PENDING, R_LIVE, R_STALE };

typedef struct {
    uint32_t f0, f1, lo, hi, crc, ins;  // its functions (the region's indices), span, and the span's CRC at the scan
    uint32_t loaded, linked;            // translated; its keys are in the lookup
    uint8_t *code;
    uint32_t off, bytes, nkey, *keys;   // keys, then each one's code
    uint32_t sd;                        // it came from the reader's SD
} Chunk;

typedef struct {
    uint32_t lo, hi, state, crc, seed;  // crc: the bytes when it went live, half the SD key of its scan
    uint32_t nfunc, ntab, npool, nchunk, from_sd;
    CsFunc *func;
    CsTab *tab;
    uint32_t *pool;
    CbHooks hk;
    uint8_t *raw;
    Chunk *chunk;
} Rgn;

static Rgn **rg;
static uint32_t nrg, caprg, nrejected, ngaps;
static const Rgn *cur;  // the region whose chunk is being linked
static struct {
    uint32_t (*get)(void *ctx, uint32_t a, uint32_t b, uint8_t **blob);
    int (*put)(void *ctx, uint32_t a, uint32_t b, const uint8_t *blob, uint32_t len);
    void *ctx;
} XB;
static struct { uint32_t scans, dir_hits, dir_stored, revived, relinks, replaced; } st;

void rgn_xcache(uint32_t (*get)(void *ctx, uint32_t a, uint32_t b, uint8_t **blob),
                int (*put)(void *ctx, uint32_t a, uint32_t b, const uint8_t *blob, uint32_t len), void *ctx) {
    XB.get = get, XB.put = put, XB.ctx = ctx;
}

static const uint32_t *rd_ptr(uint32_t v) {
    return (const uint32_t *)(uintptr_t)((v & 0x1FFFFFFFu) + RDRAM_LO + t_rdram_bias);
}
static uint32_t span_crc(uint32_t lo, uint32_t hi) { return cs_crc32(0, rd_ptr(lo), hi - lo); }

// The metas' address space: the ROM, then RDRAM from the next page, so run-time code has "ROM" offsets
static uint32_t rom_base(void) { return (ovl_rom_size() + 4095) & ~4095u; }
static uint32_t view_size(void) { return rom_base() + RDRAM_BYTES; }
static uint32_t rgn_rom(uint32_t v) { return rom_base() + (v & 0x1FFFFFFFu); }

static uint32_t **rp, nrp;  // ROM pages read so far, until comb_free

static const uint32_t *comb_page(void *ctx, uint32_t pg) {
    (void)ctx;
    uint32_t off = 4096 * pg, base = rom_base();
    if (off >= base) return rd_ptr(RDRAM_LO + (off - base));
    if (!rp) {
        nrp = base / 4096;
        rp = cs_alloc(nrp * sizeof *rp);
        memset(rp, 0, nrp * sizeof *rp);
    }
    if (!rp[pg]) {
        uint32_t *w = cs_alloc(4096);
        if (!ovl_read_page(pg, w)) os_die("rgn: cart read failed at ROM offset", off);
        rp[pg] = w;
    }
    return rp[pg];
}

static void comb_free(void) {
    for (uint32_t i = 0; rp && i < nrp; i++) cs_release(rp[i]);
    cs_release(rp);
    rp = NULL, nrp = 0;
}

static const uint32_t *scan_page(void *ctx, uint32_t pg) { return rd_ptr(((const Rgn *)ctx)->lo + 4096 * pg); }

// The fixed scan with the region's functions in by start, and seg as one more segment: what its metas are made of
typedef struct { CsResult r; uint8_t *raw; uint32_t rseg, rf0, size; } View;

static void view_build(View *v, const Rgn *R, uint32_t lo, uint32_t hi) {
    const CbCache *c = ovl_ctx();
    memset(v, 0, sizeof *v);
    v->size = view_size();
    v->rseg = c->r.nseg;
    v->r.nseg = c->r.nseg + 1;
    v->r.seg = cs_alloc(v->r.nseg * sizeof *v->r.seg);
    memcpy(v->r.seg, c->r.seg, c->r.nseg * sizeof *c->r.seg);
    v->r.seg[v->rseg] = (CsSeg){lo, rgn_rom(lo), hi - lo, CS_HOW_ICACHE, 0};
    uint32_t nf = c->r.nfunc, n = nf + R->nfunc, a = 0, b = 0, k = 0;
    v->r.nfunc = n;
    v->r.func = cs_alloc((n + 1) * sizeof *v->r.func);
    v->raw = cs_alloc(n + 1);
    while (a < nf || b < R->nfunc) {
        if (b >= R->nfunc || (a < nf && c->r.func[a].start < R->func[b].start)) {
            v->raw[k] = c->raw ? c->raw[a] : 0;
            v->r.func[k++] = c->r.func[a++];
            continue;
        }
        if (!b) v->rf0 = k;
        else if (k != v->rf0 + b) os_die("rgn: a fixed function lies inside the region, at", c->r.func[a - 1].start);
        v->raw[k] = R->raw ? R->raw[b] : 0;
        v->r.func[k++] = R->func[b++];
    }
    v->r.ntab = R->ntab, v->r.tab = R->tab;  // no meta made from a view chooses the fixed code, so its tables can wait
    v->r.entry = c->r.entry;
}

static void view_free(View *v) {
    cs_release(v->r.seg), cs_release(v->r.func), cs_release(v->raw);
    memset(v, 0, sizeof *v);
}

static int seg_has_func(const CsResult *r, const CsSeg *s) {
    uint32_t lo = 0, hi = r->nfunc;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (r->func[m].start < s->vram) lo = m + 1;
        else hi = m;
    }
    return lo < r->nfunc && r->func[lo].start - s->vram < s->size;
}

// Whether [lo, hi) holds code of a fixed segment or of a ROM overlay's run-time range
static int over_fixed(uint32_t lo, uint32_t hi) {
    const CsResult *r = &ovl_ctx()->r;
    for (uint32_t i = 0; i < r->nseg; i++) {
        const CsSeg *s = &r->seg[i];
        uint32_t a = RDRAM_LO | (s->vram & (CS_OV_IS_KEY(s->vram) ? CS_OV_MASK : 0x1FFFFFFFu));
        if (seg_has_func(r, s) && a < hi && lo < a + s->size) return 1;
    }
    return 0;
}

static Rgn *find(uint32_t v, uint32_t state) {
    for (uint32_t i = nrg; i-- > 0;)
        if (rg[i]->state == state && v - rg[i]->lo < rg[i]->hi - rg[i]->lo) return rg[i];
    return NULL;
}

static Rgn *add(uint32_t lo, uint32_t hi) {
    if (nrg == caprg) rg = cs_realloc(rg, (caprg = caprg ? 2 * caprg : 8) * sizeof *rg);
    Rgn *R = cs_alloc(sizeof *R);
    memset(R, 0, sizeof *R);
    R->lo = lo, R->hi = hi, R->state = R_PENDING;
    rg[nrg++] = R;
    return R;
}

static void forget(Rgn *R) {  // only a pending region or one without code: nothing can point into it
    for (uint32_t i = 0; i < nrg; i++)
        if (rg[i] == R) {
            memmove(&rg[i], &rg[i + 1], (nrg - i - 1) * sizeof *rg);
            nrg--;
            break;
        }
    cs_release(R->func), cs_release(R->tab), cs_release(R->pool), cs_release(R->hk.addr), cs_release(R->hk.pol);
    cs_release(R->raw), cs_release(R->chunk);
    cs_release(R);
}

static void unlink_chunk(Chunk *c) {
    for (uint32_t k = 0; k < c->nkey; k++) t_lookup_remove(c->keys[k]);
    c->linked = 0;
}

static void relink_chunk(Chunk *c) {
    for (uint32_t k = 0; k < c->nkey; k++) t_lookup_insert(c->keys[k], c->keys[c->nkey + k]);
    c->linked = 1;
}

static void stale(Rgn *R) {
    for (uint32_t k = 0; k < R->nchunk; k++)
        if (R->chunk[k].linked) unlink_chunk(&R->chunk[k]);
    R->state = R_STALE;
}

void rgn_note(uint32_t vaddr, uint32_t bytes) {
    uint32_t lo = (RDRAM_LO | (vaddr & 0x1FFFFFFFu)) & ~3u;
    if (lo >= RDRAM_HI) return;
    uint32_t hi = bytes >= RDRAM_HI - lo ? RDRAM_HI : (lo + bytes + 3) & ~3u;
    for (uint32_t i = 0; i < nrg; i++) {  // code it may have changed is checked against its scan when next looked up
        Rgn *R = rg[i];
        if (R->state != R_LIVE) continue;
        for (uint32_t k = 0; k < R->nchunk; k++) {
            Chunk *c = &R->chunk[k];
            if (c->linked && c->lo < hi && lo < c->hi) unlink_chunk(c);
        }
    }
    if (bytes < RGN_MIN) return;
    for (uint32_t i = 0; i < nrg; i++)
        if (rg[i]->state != R_STALE && rg[i]->lo == lo && rg[i]->hi == hi) return;
    if (over_fixed(lo, hi)) {
        if (nrejected++ < 4)
            cs_log("rgn: %08lx-%08lx holds fixed code, so it is no region\n", (unsigned long)lo, (unsigned long)hi);
        return;
    }
    for (uint32_t i = nrg; i-- > 0;)
        if (rg[i]->state == R_PENDING && rg[i]->lo < hi && lo < rg[i]->hi) forget(rg[i]);
    add(lo, hi);
    cs_log("rgn: %08lx-%08lx noted\n", (unsigned long)lo, (unsigned long)hi);
}

// The region's scan, as kept on the SD
static uint32_t dir_tag(const Rgn *R) {
    static const char ver[] = "rgn dir 1";
    uint32_t w[2] = {R->lo, R->hi};
    return cs_crc32(cs_crc32(0, ver, sizeof ver - 1), w, 8) ^ cache_version() ^ xl_version();
}

static uint32_t *dir_build(const Rgn *R, uint32_t *nwords) {
    uint32_t rawb = (R->nfunc + 3) & ~3u;
    uint32_t n = 10 + 2 * R->nfunc + 3 * R->ntab + R->npool + 2 * R->hk.n + rawb / 4 + 3 * R->nchunk + 1, k = 0;
    uint32_t *b = cs_alloc(4 * n);
    b[k++] = DIR_MAGIC, b[k++] = dir_tag(R), b[k++] = R->lo, b[k++] = R->hi, b[k++] = R->crc;
    b[k++] = R->nfunc, b[k++] = R->ntab, b[k++] = R->npool, b[k++] = R->hk.n, b[k++] = R->nchunk;
    for (uint32_t i = 0; i < R->nfunc; i++) b[k++] = R->func[i].start, b[k++] = R->func[i].end;
    for (uint32_t i = 0; i < R->ntab; i++) b[k++] = R->tab[i].jr, b[k++] = R->tab[i].table, b[k++] = R->tab[i].n;
    memcpy(b + k, R->pool, 4 * R->npool), k += R->npool;
    for (uint32_t i = 0; i < R->hk.n; i++) b[k++] = R->hk.addr[i], b[k++] = R->hk.pol[i];
    memset(b + k, 0, rawb);
    memcpy(b + k, R->raw, R->nfunc), k += rawb / 4;
    for (uint32_t i = 0; i < R->nchunk; i++) b[k++] = R->chunk[i].f0, b[k++] = R->chunk[i].f1, b[k++] = R->chunk[i].crc;
    b[k] = cs_crc32(0, b, 4 * k), k++;
    *nwords = k;
    return b;
}

static void chunk_spans(Rgn *R) {
    for (uint32_t i = 0; i < R->nchunk; i++) {
        Chunk *c = &R->chunk[i];
        c->lo = R->func[c->f0].start, c->hi = 0, c->ins = 0;
        for (uint32_t f = c->f0; f < c->f1; f++) {
            if (R->func[f].end > c->hi) c->hi = R->func[f].end;
            c->ins += (R->func[f].end - R->func[f].start) / 4;
        }
    }
}

// R's scan from blob b (nwords), into R; 0 unless it is whole and this region's
static int dir_parse(Rgn *R, const uint32_t *b, uint32_t n) {
    if (n < 11 || b[0] != DIR_MAGIC || b[1] != dir_tag(R) || b[2] != R->lo || b[3] != R->hi || b[4] != R->crc ||
        b[n - 1] != cs_crc32(0, b, 4 * (n - 1)))
        return 0;
    uint32_t nf = b[5], nt = b[6], np = b[7], nh = b[8], nc = b[9], rawb = (nf + 3) & ~3u, k = 10;
    if (nf > n || nt > n || np > n || nh > n || nc > nf + 1 ||
        10 + 2 * nf + 3 * nt + np + 2 * nh + rawb / 4 + 3 * nc + 1 != n)
        return 0;
    R->nfunc = nf, R->ntab = nt, R->npool = np, R->hk.n = nh, R->nchunk = nc;
    R->func = cs_alloc((nf + 1) * sizeof *R->func);
    for (uint32_t i = 0; i < nf; i++, k += 2) R->func[i] = (CsFunc){b[k], b[k + 1], CS_ORIG_CALL, 0, NULL};
    R->tab = cs_alloc((nt + 1) * sizeof *R->tab);
    R->pool = cs_alloc(4 * np + 4);
    memcpy(R->pool, b + 10 + 2 * nf + 3 * nt, 4 * np);
    uint32_t at = 0, ok = 1;
    for (uint32_t i = 0; i < nt; i++, k += 3) {
        R->tab[i] = (CsTab){b[k], b[k + 1], b[k + 2], R->pool + at};
        at += b[k + 2];
        ok &= at <= np;
    }
    k += np;
    R->hk.addr = cs_alloc(4 * nh + 4), R->hk.pol = cs_alloc(2 * nh + 2);
    for (uint32_t i = 0; i < nh; i++, k += 2) {
        R->hk.addr[i] = b[k], R->hk.pol[i] = (uint16_t)b[k + 1];
        ok &= b[k + 1] < pol_n;
    }
    R->raw = cs_alloc(nf + 1);
    memcpy(R->raw, b + k, nf), k += rawb / 4;
    R->chunk = cs_alloc((nc + 1) * sizeof *R->chunk);
    memset(R->chunk, 0, (nc + 1) * sizeof *R->chunk);
    for (uint32_t i = 0; i < nc; i++, k += 3) {
        R->chunk[i].f0 = b[k], R->chunk[i].f1 = b[k + 1], R->chunk[i].crc = b[k + 2];
        ok &= b[k] < b[k + 1] && b[k + 1] <= nf && (!i || b[k] == R->chunk[i - 1].f1);
    }
    if (!ok || at != np) return 0;
    chunk_spans(R);
    return 1;
}

static void dir_clear(Rgn *R) {
    cs_release(R->func), cs_release(R->tab), cs_release(R->pool), cs_release(R->hk.addr), cs_release(R->hk.pol);
    cs_release(R->raw), cs_release(R->chunk);
    R->func = NULL, R->tab = NULL, R->pool = NULL, R->hk.addr = NULL, R->hk.pol = NULL, R->raw = NULL, R->chunk = NULL;
    R->nfunc = R->ntab = R->npool = R->hk.n = R->nchunk = 0;
}

static int dir_get(Rgn *R) {
    uint8_t *blob = NULL;
    uint32_t len = XB.get ? XB.get(XB.ctx, R->crc, dir_tag(R), &blob) : 0;
    int ok = len && !(len & 3) && dir_parse(R, (const uint32_t *)blob, len / 4);
    if (!ok) dir_clear(R);
    cs_release(blob);
    return ok;
}

static void dir_put(const Rgn *R) {
    uint32_t n, *b = dir_build(R, &n);
    if (XB.put && XB.put(XB.ctx, R->crc, dir_tag(R), (const uint8_t *)b, 4 * n)) st.dir_stored++;
    cs_release(b);
}

// Whether the function's last jump (before any trailing nops) leaves it: a cut there needs no stub hop
static int ends_flow(uint32_t start, uint32_t end) {
    uint32_t p = end;
    for (int k = 0; k < 8 && p - 4 > start && !*rd_ptr(p - 4); k++) p -= 4;
    for (uint32_t q = p >= start + 8 ? p - 8 : start; q < p; q += 4) {
        uint32_t w = *rd_ptr(q);
        if (w == ERET || (w & 0xFC1FFFFFu) == 0x00000008u || w >> 26 == 2 || w >> 16 == 0x1000) return 1;
    }
    return 0;
}

static void cut(Rgn *R) {
    R->chunk = cs_alloc((R->nfunc + 1) * sizeof *R->chunk);
    memset(R->chunk, 0, (R->nfunc + 1) * sizeof *R->chunk);
    R->nchunk = 0;
    for (uint32_t f = 0; f < R->nfunc;) {
        Chunk *c = &R->chunk[R->nchunk++];
        uint32_t ins = 0;
        c->f0 = f;
        while (f < R->nfunc) {
            const CsFunc *fn = &R->func[f++];
            ins += (fn->end - fn->start) / 4;
            if (ins >= CHUNK_INS && ends_flow(fn->start, fn->end)) break;
        }
        c->f1 = f;
    }
    chunk_spans(R);
    for (uint32_t i = 0; i < R->nchunk; i++) R->chunk[i].crc = span_crc(R->chunk[i].lo, R->chunk[i].hi);
}

static void scan(Rgn *R) {
    uint64_t t0 = xl_now_us(), t1, t2;
    uint32_t size = R->hi - R->lo;
    Scan *s = cs_new(size, scan_page, R);
    cs_feed(s, rd_ptr(R->lo), size / 4);
    const CsResult *r = cs_run_region(s, R->lo, &R->seed, 1);
    cs_drop_work(s);
    R->nfunc = r->nfunc, R->ntab = r->ntab, R->npool = 0;
    R->func = cs_alloc((r->nfunc + 1) * sizeof *R->func);
    for (uint32_t i = 0; i < r->nfunc; i++) R->func[i] = (CsFunc){r->func[i].start, r->func[i].end, r->func[i].origin, 0, NULL};
    for (uint32_t i = 0; i < r->ntab; i++) R->npool += r->tab[i].n;
    R->tab = cs_alloc((r->ntab + 1) * sizeof *R->tab);
    R->pool = cs_alloc(4 * R->npool + 4);
    for (uint32_t i = 0, at = 0; i < r->ntab; at += r->tab[i++].n) {
        memcpy(R->pool + at, r->tab[i].targets, 4 * r->tab[i].n);
        R->tab[i] = (CsTab){r->tab[i].jr, r->tab[i].table, r->tab[i].n, R->pool + at};
    }
    cs_free(s);
    t1 = xl_now_us();
    CbCache *c = ovl_ctx();
    if (!c->raw) c->raw = cb_raw_bits(&c->r, &c->hk, comb_page, NULL, view_size());  // a cart with no overlays
    uint32_t clo = R->nfunc ? R->func[0].start : R->lo, chi = clo;  // the code's span, not the data the game invalidated
    for (uint32_t i = 0; i < R->nfunc; i++)
        if (R->func[i].end > chi) chi = R->func[i].end;
    View v;
    view_build(&v, R, clo, chi);
    HmResult *h = hm_run_seg(&v.r, (int)v.rseg, comb_page, NULL, v.size);
    CbHooks hk;
    hooks_from_hm(h, &hk);
    hm_free(h);
    R->hk.addr = cs_alloc(4 * hk.n + 4), R->hk.pol = cs_alloc(2 * hk.n + 2), R->hk.n = 0;
    for (uint32_t i = 0; i < hk.n; i++)
        if (hk.addr[i] - R->lo < R->hi - R->lo) R->hk.addr[R->hk.n] = hk.addr[i], R->hk.pol[R->hk.n++] = hk.pol[i];
    cs_release(hk.addr), cs_release(hk.pol);
    t2 = xl_now_us();
    uint32_t len;
    uint8_t *meta = meta_write_seg(&v.r, &R->hk, v.raw, (int)v.rseg, comb_page, NULL, v.size, &len);
    uint8_t *part = cs_alloc(v.r.nfunc + 1);
    uint32_t n = xl_raw_pass(meta, len, part);
    xl_release();
    cs_release(meta);
    if (n != R->nfunc) os_die("rgn: the raw pass counted other functions than the scan, of", R->nfunc);
    R->raw = cs_alloc(R->nfunc + 1);
    memcpy(R->raw, part, R->nfunc);
    cs_release(part);
    view_free(&v);
    comb_free();
    cut(R);
    st.scans++;
    cs_log("rgn: ms scan %lu sigs %lu raw %lu\n", (unsigned long)((t1 - t0) / 1000), (unsigned long)((t2 - t1) / 1000),
           (unsigned long)((xl_now_us() - t2) / 1000));
}

static void revive(Rgn *R) {
    for (uint32_t k = 0; k < R->nchunk; k++)
        if (R->chunk[k].loaded && !R->chunk[k].linked) relink_chunk(&R->chunk[k]);
    R->state = R_LIVE;
}

// P's first lookup: the same bytes as a region seen before bring that one back, else its scan comes from the SD or
// is made here
static Rgn *activate(Rgn *P, uint32_t v) {
    uint64_t t0 = xl_now_us();
    P->seed = v;
    P->crc = span_crc(P->lo, P->hi);
    for (uint32_t i = 0; i < nrg; i++)
        if (rg[i] != P && rg[i]->state == R_LIVE && rg[i]->lo < P->hi && P->lo < rg[i]->hi) stale(rg[i]);
    for (uint32_t i = 0; i < nrg; i++) {
        Rgn *R = rg[i];
        if (R->state == R_STALE && R->lo == P->lo && R->hi == P->hi && R->crc == P->crc) {
            forget(P);
            revive(R);
            st.revived++;
            cs_log("rgn: %08lx-%08lx holds the same bytes as before: its translations are back\n", (unsigned long)R->lo,
                   (unsigned long)R->hi);
            return R;
        }
    }
    P->from_sd = dir_get(P);
    if (P->from_sd) st.dir_hits++;
    else scan(P), dir_put(P);
    P->state = R_LIVE;
    uint32_t ins = 0;
    for (uint32_t i = 0; i < P->nfunc; i++) ins += (P->func[i].end - P->func[i].start) / 4;
    cs_log("rgn: %08lx-%08lx from %08lx: %lu functions, %lu instructions, %lu jump tables, %lu hooks, %lu chunks, %s "
           "in %lu ms\n",
           (unsigned long)P->lo, (unsigned long)P->hi, (unsigned long)v, (unsigned long)P->nfunc, (unsigned long)ins,
           (unsigned long)P->ntab, (unsigned long)P->hk.n, (unsigned long)P->nchunk,
           P->from_sd ? "its scan from the reader's SD" : "scanned", (unsigned long)((xl_now_us() - t0) / 1000));
    return P;
}

// Fixed code, and the region being linked: by address. Other regions' code may be replaced, so it is always looked up
static uint32_t lookup(uint32_t v) {
    if (cur && v - cur->lo < cur->hi - cur->lo) return t_lookup_exact(v);
    for (uint32_t i = 0; i < nrg; i++)
        if (v - rg[i]->lo < rg[i]->hi - rg[i]->lo) return 0;
    return t_lookup_exact(v);
}

static void load(Rgn *R, Chunk *c) {
    uint64_t t0 = xl_now_us();
    View v;
    view_build(&v, R, c->lo, c->hi);
    uint32_t mlen, *keys, nkey;
    uint8_t *meta = meta_write_seg(&v.r, &R->hk, v.raw, (int)v.rseg, comb_page, NULL, v.size, &mlen);
    view_free(&v);
    comb_free();
    XlStats xs;
    cur = R;
    c->code = ovl_link_meta(meta, mlen, c->ins, 1, WHO, c->lo, ovl_place_heap, lookup, &c->off, &c->bytes, &keys, &nkey,
                            &xs);
    cur = NULL;
    cs_release(meta);
    c->nkey = nkey;
    c->keys = cs_alloc(8 * nkey + 4);
    for (uint32_t k = 0; k < nkey; k++) c->keys[k] = keys[k], c->keys[nkey + k] = t_lookup_exact(keys[k]);
    cs_release(keys);
    c->loaded = c->linked = 1, c->sd = !xs.mips;
    cs_log("rgn: chunk %lu of %08lx: %08lx-%08lx, %lu instructions, %lu KB at %08lx, %s in %lu ms\n",
           (unsigned long)(c - R->chunk), (unsigned long)R->lo, (unsigned long)c->lo, (unsigned long)c->hi,
           (unsigned long)c->ins, (unsigned long)(c->bytes >> 10), (unsigned long)(uintptr_t)c->code,
           c->sd ? "from the reader's SD" : "translated", (unsigned long)((xl_now_us() - t0) / 1000));
}

static Chunk *chunk_of(Rgn *R, uint32_t v) {
    uint32_t lo = 0, hi = R->nchunk;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (R->chunk[m].lo <= v) lo = m + 1;
        else hi = m;
    }
    return lo && v < R->chunk[lo - 1].hi ? &R->chunk[lo - 1] : NULL;
}

static void gap(uint32_t v) {
    if (ngaps++ < 16) cs_log("rgn: no function at %08lx in its region's scan; one function at a time\n", (unsigned long)v);
}

static void work(uint32_t v) {
    Rgn *R = find(v, R_PENDING);
    R = R ? activate(R, v) : find(v, R_LIVE);
    for (int tries = 0; R && tries < 2; tries++) {
        Chunk *c = chunk_of(R, v);
        if (!c || c->linked) {
            gap(v);
            return;
        }
        if (span_crc(c->lo, c->hi) == c->crc) {
            if (c->loaded) relink_chunk(c), st.relinks++;
            else load(R, c);
            return;
        }
        st.replaced++;
        cs_log("rgn: %08lx-%08lx changed under chunk %lu: scanned again\n", (unsigned long)R->lo, (unsigned long)R->hi,
               (unsigned long)(c - R->chunk));
        Rgn *P = add(R->lo, R->hi);
        stale(R);
        R = activate(P, v);
    }
}

uint32_t rgn_code(uint32_t vram) {
    uint32_t v = RDRAM_LO | (vram & 0x1FFFFFFFu);
    if (!find(v, R_PENDING) && !find(v, R_LIVE)) return 0;
    os_big_call(work, v);
    uint32_t fn = t_lookup_exact(v);
    if (!fn) return 0;
    if (vram != v) t_lookup_insert(vram, fn);
    return fn;
}

uint32_t rgn_guest_fn(uint32_t h) {
    for (uint32_t i = 0; i < nrg; i++)
        for (uint32_t k = 0; k < rg[i]->nchunk; k++) {
            const Chunk *c = &rg[i]->chunk[k];
            if (!c->loaded || h - (uint32_t)(uintptr_t)c->code >= c->bytes) continue;
            return os_fn_below(c->keys, c->keys + c->nkey, 1, c->nkey, h, NULL);
        }
    return 0;
}

// One function the game writes into RDRAM and runs outside any region (Kirby 64 decrypts a check into an overlay's
// BSS): translated from RDRAM as a one-function segment keyed in the overlay slot ovkey.h reserves, "ROM" pages RDRAM's
#define FN_ROM 0x40000000u
#define FN_MAX 16

typedef struct { uint32_t lo, hi, key, crc, live, who, off, len, nkey, *keys; } Fn;
static Fn fns[FN_MAX];
static uint32_t nfn, fn_who = CS_OV_ROM_MAX, fn_key, fn_code, fn_pg[1024];

static const uint32_t *fn_page(void *ctx, uint32_t pg) {
    if (pg < FN_ROM >> 12) return ovl_page(ctx, pg);
    for (uint32_t i = 0; i < 1024; i++) fn_pg[i] = *rd_ptr(RDRAM_LO + 4096 * (pg - (FN_ROM >> 12)) + 4 * i);
    return fn_pg;
}

// The end of the function at v: every path followed to its jr, j, b or eret, j targets too unless tail_j; 0 if one
// runs past RGN_MIN. *holes: words in the span no path reaches, so data (a fragment header after its entry's j)
static uint32_t fn_walk(uint32_t v, int tail_j, int *holes) {
    static uint8_t seen[RGN_MIN / 4];
    uint32_t todo[64], nt = 0, end = v;
    memset(seen, 0, sizeof seen);
    todo[nt++] = v;
    while (nt) {
        for (uint32_t a = todo[--nt];;) {
            if (a - v >= RGN_MIN || (a & 0x1FFFFFFFu) >= RDRAM_HI - RDRAM_LO) return 0;
            if (seen[(a - v) / 4]) break;
            seen[(a - v) / 4] = 1;
            uint32_t w = *rd_ptr(a), op = w >> 26, rt = (w >> 16) & 31;
            if (a + 4 > end) end = a + 4;
            int branch = (op >= 4 && op <= 7) || (op >= 0x14 && op <= 0x17) || (op == 1 && !(rt & 0xC)) ||
                         (op == 0x11 && ((w >> 21) & 31) == 8);
            int stop = (w >> 16) == 0x1000 || op == 2 || (op == 0 && (w & 63) == 8);  // b, j, jr: past the slot
            if (branch) {
                uint32_t tg = a + 4 + ((uint32_t)(int32_t)(int16_t)w << 2);
                if (tg - v < RGN_MIN && nt < 64) todo[nt++] = tg;
            } else if (op == 2 && !tail_j) {
                uint32_t tg = (a & 0xF0000000u) | (w & 0x3FFFFFFu) << 2;
                if (tg - v < RGN_MIN && nt < 64) todo[nt++] = tg;
            }
            if (stop) {
                if (a + 8 > end) end = a + 8;
                if (a + 4 - v < RGN_MIN) seen[(a + 4 - v) / 4] = 1;  // the delay slot
                break;
            }
            if (w == ERET) break;
            a += 4;
        }
    }
    *holes = 0;
    for (uint32_t a = v; a < end && !*holes; a += 4) *holes = !seen[(a - v) / 4];
    return end;
}

// A j over data is a tail call: the function stops at its slot, and the target is its own function
static uint32_t fn_extent(uint32_t v) {
    int holes;
    uint32_t end = fn_walk(v, 0, &holes);
    return end && holes ? fn_walk(v, 1, &holes) : end;
}

static void fn_unlink(Fn *m) {
    for (uint32_t k = 0; k < m->nkey; k++) t_lookup_remove(m->keys[k]);
    t_lookup_remove(m->key);
    m->live = 0;
}

static void fn_drop(uint32_t i) {
    if (fns[i].live) fn_unlink(&fns[i]);
    cs_release(fns[i].keys);
    ovl_arena_drop(fns[i].who);
    memmove(&fns[i], &fns[i + 1], (nfn - i - 1) * sizeof *fns);
    nfn--;
}

static void fn_translate(uint32_t vram) {
    uint32_t lo = RDRAM_LO | (vram & 0x1FFFFFFFu), hi = fn_extent(lo), crc = hi ? span_crc(lo, hi) : 0;
    fn_code = 0;
    if (!hi) return;
    for (uint32_t i = 0; i < nfn; i++)  // the same bytes again (written and invalidated anew): relink, don't retranslate
        if (!fns[i].live && fns[i].lo == lo && fns[i].hi == hi && fns[i].crc == crc) {
            Fn *m = &fns[i];
            for (uint32_t k = 0; k < m->nkey; k++) t_lookup_insert(m->keys[k], m->keys[k + m->nkey]);
            m->key = fn_key, m->live = 1;
            fn_code = t_lookup_exact(CS_OV_RAM_KEY | (lo & CS_OV_MASK));
            if (fn_code) t_lookup_insert(m->key, fn_code);
            return;
        }
    for (uint32_t i = 0; i < nfn;)
        if (!fns[i].live && fns[i].lo < hi && lo < fns[i].hi) fn_drop(i);
        else i++;
    if (nfn == FN_MAX) fn_drop(0);
    uint64_t t0 = xl_now_us();
    const CbCache *c = ovl_ctx();
    CsResult r = c->r;
    r.seg = cs_alloc((r.nseg + 1) * sizeof *r.seg);
    r.func = cs_alloc((r.nfunc + 1) * sizeof *r.func);
    memcpy(r.seg, c->r.seg, r.nseg * sizeof *r.seg);
    memcpy(r.func, c->r.func, r.nfunc * sizeof *r.func);
    uint8_t *raw = cs_alloc(r.nfunc + 1);
    if (c->raw) memcpy(raw, c->raw, r.nfunc);
    else memset(raw, 0, r.nfunc);
    uint32_t key = CS_OV_RAM_KEY | (lo & CS_OV_MASK);
    r.seg[r.nseg++] = (CsSeg){key, FN_ROM + (lo & CS_OV_MASK), hi - lo, CS_HOW_TABLE, 0};
    r.func[r.nfunc] = (CsFunc){key, key + (hi - lo), CS_ORIG_CALL, 0, NULL};
    raw[r.nfunc++] = 0;
    uint32_t mlen;
    uint8_t *meta = meta_write_seg(&r, &c->hk, raw, (int)r.nseg - 1, fn_page, NULL, FN_ROM + CS_OV_SPAN, &mlen);
    cs_release(raw), cs_release(r.func), cs_release(r.seg);
    Fn *m = &fns[nfn++];
    memset(m, 0, sizeof *m);
    m->lo = lo, m->hi = hi, m->crc = crc, m->who = fn_who++;
    XlStats xs;
    uint32_t *keys, nkey;
    uint8_t *base = ovl_link_meta(meta, mlen, (hi - lo) / 4, 0, m->who, lo, ovl_place_ram, t_lookup_exact, &m->off,
                                  &m->len, &keys, &nkey, &xs);
    cs_release(meta);
    m->nkey = nkey;
    m->keys = cs_alloc(8 * nkey + 4);  // the keys, then each one's code
    for (uint32_t k = 0; k < nkey; k++) m->keys[k] = keys[k], m->keys[k + nkey] = t_lookup_exact(keys[k]);
    cs_release(keys);
    m->key = fn_key, m->live = 1;
    fn_code = t_lookup_exact(key);
    if (fn_code) t_lookup_insert(m->key, fn_code);
    cs_log("rgn: run-time function %08lx-%08lx, %lu instructions in %lu ms, code at %08lx\n", (unsigned long)lo,
           (unsigned long)hi, (unsigned long)xs.mips, (unsigned long)((xl_now_us() - t0) / 1000),
           (unsigned long)(uintptr_t)base);
}

static int zero_span(uint32_t a, uint32_t n) {
    const uint32_t *p = rd_ptr(a);
    for (uint32_t i = 0; i < n / 4; i++)
        if (p[i]) return 0;
    return 1;
}

// Fixed code's jal into RDRAM no segment holds (THPS2's loader calls what it unpacked, with no osInvalICache): a region
// over the nonzero RAM around it, between fixed code and all-zero RGN_MIN blocks
static void note_unplaced(uint32_t v) {
    const CsResult *r = &ovl_ctx()->r;
    uint32_t i = 0;
    while (i < r->nunplaced && (RDRAM_LO | (r->unplaced[i] & 0x1FFFFFFFu)) != v) i++;
    if (i == r->nunplaced || find(v, R_PENDING) || find(v, R_LIVE)) return;
    uint32_t floor = RDRAM_LO, ceil = RDRAM_HI;
    for (uint32_t k = 0; k < r->nseg; k++) {
        const CsSeg *s = &r->seg[k];
        if (CS_OV_IS_KEY(s->vram) || !seg_has_func(r, s)) continue;
        uint32_t a = RDRAM_LO | (s->vram & 0x1FFFFFFFu), e = a + s->size;
        if (e <= v && e > floor) floor = e;
        if (a > v && a < ceil) ceil = a;
    }
    uint32_t lo = v & ~(RGN_MIN - 1), hi = lo + RGN_MIN;
    while (lo > floor && lo - RGN_MIN >= floor && !zero_span(lo - RGN_MIN, RGN_MIN)) lo -= RGN_MIN;
    if (lo > floor && lo - floor < RGN_MIN && !zero_span(floor, lo - floor)) lo = floor;
    if (lo < floor) lo = floor;
    while (hi < ceil && !zero_span(hi, ceil - hi < RGN_MIN ? ceil - hi : RGN_MIN)) hi += RGN_MIN;
    if (hi > ceil) hi = ceil;
    rgn_note(lo, hi - lo);
}

// t_lookup's miss: a region's chunk, else the one function at vram, translated
static uint32_t ram_code(uint32_t vram, uint32_t key) {
    uint32_t fn = t_lookup_exact(key);
    if (fn) return fn;
    if ((vram >> 30) != 2 || (vram & 0x1FFFFFFFu) >= RDRAM_HI - RDRAM_LO) return 0;  // KSEG0 or KSEG1 RDRAM only
    if (key == vram) note_unplaced(RDRAM_LO | (vram & 0x1FFFFFFFu));
    if (key == vram && (fn = rgn_code(vram))) return fn;
    fn_key = key;
    os_big_call(fn_translate, vram);
    return fn_code;
}

// osInvalICache: run-time translations over the range are stale until the game runs them again
static void ram_written(uint32_t vaddr, uint32_t bytes) {
    rgn_note(vaddr, bytes);
    uint32_t lo = RDRAM_LO | (vaddr & 0x1FFFFFFFu), all = bytes >= RGN_MIN;
    for (uint32_t i = 0; i < nfn; i++)
        if (fns[i].live && (all || (fns[i].lo < lo + bytes && lo < fns[i].hi))) fn_unlink(&fns[i]);
}

uint32_t rgn_any_guest_fn(uint32_t h) {
    uint32_t f = ovl_guest_fn(h);
    return f ? f : rgn_guest_fn(h);
}

void rgn_init(void) {
    t_ram_code = ram_code;
    os_hooks.icache_inval = ram_written;
}

void rgn_report(void) {
    static const char *const state[] = {"pending", "live", "stale"};
    for (uint32_t i = 0; i < nrg; i++) {
        const Rgn *R = rg[i];
        uint32_t n = 0, sd = 0, kb = 0, ins = 0, all = 0;
        for (uint32_t k = 0; k < R->nchunk; k++) {
            all += R->chunk[k].ins;
            if (!R->chunk[k].loaded) continue;
            n++, sd += R->chunk[k].sd, kb += R->chunk[k].bytes, ins += R->chunk[k].ins;
        }
        cs_log("rgn: %08lx-%08lx %s: %lu of %lu chunks translated (%lu from the SD), %lu of %lu instructions, %lu KB\n",
               (unsigned long)R->lo, (unsigned long)R->hi, state[R->state], (unsigned long)n, (unsigned long)R->nchunk,
               (unsigned long)sd, (unsigned long)ins, (unsigned long)all, (unsigned long)(kb >> 10));
    }
    if (nrg)
        cs_log("rgn: %lu scans, %lu scans from the SD, %lu stored, %lu revived, %lu relinked, %lu replaced, %lu gaps\n",
               (unsigned long)st.scans, (unsigned long)st.dir_hits, (unsigned long)st.dir_stored,
               (unsigned long)st.revived, (unsigned long)st.relinks, (unsigned long)st.replaced, (unsigned long)ngaps);
}
