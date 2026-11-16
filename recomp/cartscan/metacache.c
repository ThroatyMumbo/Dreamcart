// The cache blob: everything the scan and the signatures decided, keyed by the ROM's size and header, and by a
// version that changes with the pipeline and its signature tables
#include "boot.h"

#include <string.h>

#include "buf.h"

#define CACHE_MAGIC 0x31434243u  // "CBC1"

static uint32_t crc_u32(uint32_t c, uint32_t v) {
    uint8_t le[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return cs_crc32(c, le, 4);
}

uint32_t cache_version(void) {
    static const char ver[] = "cartboot cache 12";  // bump it when the scan changes
    uint32_t c = cs_crc32(0, ver, sizeof ver - 1), nv = 0, nr = 0;
    for (uint32_t i = 0; i < sig_nsyms; i++) {
        const SigSym *s = &sig_syms[i];
        c = crc_u32(crc_u32(crc_u32(crc_u32(c, s->name), s->var0), s->nvar), s->leaf);
        if (s->var0 + s->nvar > nv) nv = s->var0 + s->nvar;
    }
    for (uint32_t i = 0; i < nv; i++) {
        const SigVar *v = &sig_vars[i];
        c = crc_u32(crc_u32(crc_u32(crc_u32(crc_u32(crc_u32(c, v->numops), v->ref0), v->nref), v->firstop), v->partial),
                    v->crc);
        if (v->ref0 + v->nref > nr) nr = v->ref0 + v->nref;
    }
    for (uint32_t i = 0; i < nr; i++) c = crc_u32(crc_u32(crc_u32(c, sig_refs[i].off), sig_refs[i].target), sig_refs[i].kind);
    for (uint32_t i = 0; i < sig_nnames; i++) c = cs_crc32(c, sig_names[i], strlen(sig_names[i]) + 1);
    for (uint32_t i = 0; i < pol_n; i++)
        c = cs_crc32(cs_crc32(c, pol_names[i], strlen(pol_names[i]) + 1), pol_hooks[i], strlen(pol_hooks[i]) + 1);
    return c;
}

uint8_t *cache_build(const CsResult *scan, const HmResult *hm, const uint8_t *raw, uint32_t rom_size,
                     const uint32_t header[16], uint32_t *len) {
    CbHooks hk;
    hooks_from_hm(hm, &hk);
    uint32_t need = 4 * (3 + 16 + 4 + 1 + 3 * scan->nseg + 1 + 2 * scan->nfunc + 1 + 2 * hk.n + 1 + 1 + 1) +
                    scan->nfunc + 4;
    for (uint32_t i = 0; i < scan->ntab; i++) need += 4 * (3 + scan->tab[i].n);
    Buf o = {cs_alloc(need), 0, need};
    u32(&o, CACHE_MAGIC), u32(&o, cache_version()), u32(&o, rom_size);
    for (int i = 0; i < 16; i++) u32(&o, header[i]);
    u32(&o, hm->os_thread_tail), u32(&o, hm->os_event_tab), u32(&o, hm->os_rcp_im_table), u32(&o, hm->os_destroy_thread);
    u32(&o, scan->nseg);
    for (uint32_t i = 0; i < scan->nseg; i++) u32(&o, scan->seg[i].vram), u32(&o, scan->seg[i].rom), u32(&o, scan->seg[i].size);
    u32(&o, scan->nfunc);
    for (uint32_t i = 0; i < scan->nfunc; i++) u32(&o, scan->func[i].start), u32(&o, scan->func[i].end);
    u32(&o, scan->ntab);
    for (uint32_t i = 0; i < scan->ntab; i++) {
        u32(&o, scan->tab[i].jr), u32(&o, scan->tab[i].table), u32(&o, scan->tab[i].n);
        for (uint32_t k = 0; k < scan->tab[i].n; k++) u32(&o, scan->tab[i].targets[k]);
    }
    u32(&o, hk.n);
    for (uint32_t i = 0; i < hk.n; i++) u32(&o, hk.addr[i]), u32(&o, hk.pol[i]);
    u32(&o, raw != NULL);
    if (raw) {
        static const uint8_t z[4];
        put(&o, raw, scan->nfunc);
        put(&o, z, (4 - scan->nfunc % 4) % 4);
    }
    u32(&o, cs_crc32(0, o.b, o.n));
    cs_release(hk.addr), cs_release(hk.pol);
    *len = o.n;
    return o.b;
}

static uint32_t rd(const uint8_t *b) { return (uint32_t)b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

// 1 and v filled when blob is a valid cache for this ROM and this pipeline; v points into blob
int cache_parse(const uint8_t *blob, uint32_t len, uint32_t rom_size, const uint32_t header[16], CbCache *v) {
    memset(v, 0, sizeof *v);
    if (len < 4 * 30 || len % 4 || rd(blob) != CACHE_MAGIC || rd(blob + len - 4) != cs_crc32(0, blob, len - 4)) return 0;
    const uint8_t *p = blob + 4, *end = blob + len - 4;
#define NEXT() (p + 4 <= end ? (p += 4, rd(p - 4)) : (ok = 0, 0u))
    int ok = 1;
    if (NEXT() != cache_version() || NEXT() != rom_size) return 0;
    for (int i = 0; i < 16; i++)
        if (NEXT() != header[i]) return 0;
    for (int i = 0; i < 4; i++) v->os[i] = NEXT();
    v->r.nseg = NEXT();
    if (!ok || v->r.nseg > 64) return 0;
    v->r.seg = cs_alloc((v->r.nseg + 1) * sizeof *v->r.seg);
    for (uint32_t i = 0; i < v->r.nseg && ok; i++) v->r.seg[i].vram = NEXT(), v->r.seg[i].rom = NEXT(), v->r.seg[i].size = NEXT();
    v->r.nfunc = NEXT();
    if (!ok || v->r.nfunc > (uint32_t)(end - p) / 8) return 0;
    v->r.func = cs_alloc((v->r.nfunc + 1) * sizeof *v->r.func);
    for (uint32_t i = 0; i < v->r.nfunc && ok; i++) {
        memset(&v->r.func[i], 0, sizeof v->r.func[i]);
        v->r.func[i].start = NEXT(), v->r.func[i].end = NEXT();
    }
    v->r.ntab = NEXT();
    if (!ok || v->r.ntab > (uint32_t)(end - p) / 12) return 0;
    v->r.tab = cs_alloc((v->r.ntab + 1) * sizeof *v->r.tab);
    v->r.pool = cs_alloc(len);
    uint32_t *pool = v->r.pool;
    for (uint32_t i = 0; i < v->r.ntab && ok; i++) {
        v->r.tab[i].jr = NEXT(), v->r.tab[i].table = NEXT(), v->r.tab[i].n = NEXT();
        if (v->r.tab[i].n > (uint32_t)(end - p) / 4) return 0;
        v->r.tab[i].targets = pool;
        for (uint32_t k = 0; k < v->r.tab[i].n; k++) *pool++ = NEXT();
    }
    v->hk.n = NEXT();
    if (!ok || v->hk.n > (uint32_t)(end - p) / 8) return 0;
    v->hk.addr = cs_alloc((v->hk.n + 1) * 4), v->hk.pol = cs_alloc((v->hk.n + 1) * 2);
    for (uint32_t i = 0; i < v->hk.n && ok; i++) {
        v->hk.addr[i] = NEXT();
        uint32_t pol = NEXT();
        if (pol >= pol_n) return 0;
        v->hk.pol[i] = (uint16_t)pol;
    }
    if (NEXT() && ok) {
        uint32_t padded = (v->r.nfunc + 3) & ~3u;
        if ((uint32_t)(end - p) < padded) return 0;
        v->raw = cs_alloc(v->r.nfunc + 1);
        memcpy(v->raw, p, v->r.nfunc);
        p += padded;
    }
#undef NEXT
    return ok && p == end;
}

