#include "xlpack.h"

#include <string.h>

#include "xlz.h"

#ifndef XL_VER
#error "XL_VER: the build passes a checksum of the translator's and packer's sources (XL_SRCS)"
#endif

#define XLP_MAGIC 0x31504C58u  // "XLP1"

enum { H_MAGIC, H_VER, H_META_CRC, H_META_LEN, H_TOTAL, H_LEN, H_NREL, H_NFUNCS, H_NUND, H_NOVL, H_FUNCTAB,
       H_FUNCTAB_COUNT, H_CALL, H_NAMES, H_SEC, H_CRC = XLP_HDR / 4 - 1 };
enum { S_TEXT, S_REL, S_FUNCS, S_FUNC_OFF, S_NAMES, S_OVTAB };

uint32_t xl_version(void) { return XL_VER; }

uint32_t xl_pack_key(uint32_t meta_len) {
    uint8_t b[4] = {meta_len, meta_len >> 8, meta_len >> 16, meta_len >> 24};
    return xl_crc32(xl_version(), b, 4);
}

static uint32_t hdr_crc(const uint32_t *h) { return xl_crc32(0, h, 4 * H_CRC); }

static void layout(uint32_t *h, uint32_t *lens) {
    lens[S_TEXT] = h[H_LEN];
    lens[S_REL] = 8 * h[H_NREL];
    lens[S_FUNCS] = lens[S_FUNC_OFF] = 4 * h[H_NFUNCS];
    lens[S_NAMES] = h[H_NAMES];
    lens[S_OVTAB] = 16 * h[H_NOVL];
    uint32_t at = XLP_HDR;
    for (uint32_t i = 0; i < XLP_NSEC; i++) h[H_SEC + i] = at, at += lens[i];
    h[H_TOTAL] = at;
}

int xl_pack(const XlOut *o, uint32_t meta_crc, uint32_t meta_len, XlPack *p) {
    memset(p, 0, sizeof *p);
    if ((o->len & 3) || o->nfuncs >= 1u << 24 || o->nund >= 1u << 24) return 0;
    uint32_t names = 0;
    for (uint32_t i = 0; i < o->nund; i++) names += (uint32_t)strlen(o->und[i]) + 1;
    names = (names + 3) & ~3u;
    p->rel = xl_alloc(8 * o->nrel + 4);
    p->names = xl_alloc(names + 4);
    if (!p->rel || !p->names) return 0;
    for (uint32_t i = 0; i < o->nrel; i++) {
        const XlReloc *r = &o->rel[i];
        if (r->id >= 1u << 24) return 0;
        p->rel[2 * i] = r->off, p->rel[2 * i + 1] = (uint32_t)r->kind << 24 | r->id;
    }
    memset(p->names, 0, names);
    char *s = (char *)p->names;
    for (uint32_t i = 0; i < o->nund; i++) {
        size_t n = strlen(o->und[i]) + 1;
        memcpy(s, o->und[i], n);
        s += n;
    }
    uint32_t *h = p->hdr, lens[XLP_NSEC];
    h[H_MAGIC] = XLP_MAGIC, h[H_VER] = xl_version(), h[H_META_CRC] = meta_crc, h[H_META_LEN] = meta_len;
    h[H_LEN] = o->len, h[H_NREL] = o->nrel, h[H_NFUNCS] = o->nfuncs, h[H_NUND] = o->nund, h[H_NOVL] = o->novl;
    h[H_FUNCTAB] = o->functab_off, h[H_FUNCTAB_COUNT] = o->functab_count_off, h[H_CALL] = o->call_off;
    h[H_NAMES] = names;
    layout(h, lens);
    h[H_CRC] = hdr_crc(h);
    const void *src[XLP_NSEC] = {o->text, p->rel, o->funcs, o->func_off, p->names, o->ovtab};
    p->crc = xl_crc32(0, h, XLP_HDR);
    for (uint32_t i = 0; i < XLP_NSEC; i++) {
        p->sec[i] = (XlpOut){h[H_SEC + i], lens[i], src[i]};
        p->crc = xl_crc32(p->crc, src[i], lens[i]);
    }
    p->total = h[H_TOTAL];
    return 1;
}

void xl_pack_free(XlPack *p) {
    xl_free(p->rel), xl_free(p->names);
    p->rel = p->names = NULL;
}

int xl_unpack_begin(const uint32_t *hdr, uint32_t meta_crc, uint32_t meta_len, XlOut *o, XlpIn in[XLP_NSEC]) {
    memset(o, 0, sizeof *o);
    memset(in, 0, XLP_NSEC * sizeof *in);
    uint32_t h[XLP_HDR / 4], lens[XLP_NSEC];
    memcpy(h, hdr, XLP_HDR);
    if (h[H_MAGIC] != XLP_MAGIC || h[H_VER] != xl_version() || h[H_META_CRC] != meta_crc ||
        h[H_META_LEN] != meta_len || h[H_CRC] != hdr_crc(h) || (h[H_LEN] & 3) || (h[H_NAMES] & 3))
        return 0;
    uint32_t sec[XLP_NSEC], total = h[H_TOTAL];
    memcpy(sec, h + H_SEC, sizeof sec);
    layout(h, lens);
    if (memcmp(sec, h + H_SEC, sizeof sec) || total != h[H_TOTAL]) return 0;
    o->len = o->cap = h[H_LEN];
    o->nrel = o->caprel = h[H_NREL];
    o->nfuncs = h[H_NFUNCS], o->nund = h[H_NUND], o->novl = h[H_NOVL];
    o->functab_off = h[H_FUNCTAB], o->functab_count_off = h[H_FUNCTAB_COUNT], o->call_off = h[H_CALL];
    if (o->functab_off >= o->len || o->functab_count_off >= o->len || o->call_off >= o->len) return 0;
    o->text = xl_alloc(o->len + 4);
    o->rel = xl_alloc(o->nrel * sizeof(XlReloc) + 16);
    uint32_t *funcs = xl_alloc(4 * o->nfuncs + 4);
    o->funcs = funcs;
    o->func_off = xl_alloc(4 * o->nfuncs + 4);
    o->ovtab = xl_alloc(16 * o->novl + 4);
    o->und = xl_alloc(o->nund * sizeof *o->und + 4);
    void *names = xl_alloc(h[H_NAMES] + 4);
    in[S_NAMES].dst = names;
    if (!o->text || !o->rel || !funcs || !o->func_off || !o->ovtab || !o->und || !names) {
        if (!o->und) o->nund = 0;
        return 0;
    }
    memset(o->und, 0, o->nund * sizeof *o->und);
    // the packed relocations go at the top of their array, so expanding them from the bottom never overtakes them
    void *dst[XLP_NSEC] = {o->text, (uint8_t *)o->rel + (sizeof(XlReloc) - 8) * o->nrel, funcs, o->func_off, names,
                           o->ovtab};
    for (uint32_t i = 0; i < XLP_NSEC; i++) in[i] = (XlpIn){h[H_SEC + i], lens[i], dst[i]};
    return 1;
}

int xl_unpack_end(XlOut *o, XlpIn in[XLP_NSEC]) {
    const uint32_t *pr = in[S_REL].dst;
    for (uint32_t i = 0; i < o->nrel; i++) {
        uint32_t off = pr[2 * i], w = pr[2 * i + 1], kind = w >> 24, id = w & 0xFFFFFF;
        if (o->len < 4 || off > o->len - 4 || kind > XL_SYM_UND || (kind == XL_SYM_FUNC && id >= o->nfuncs) ||
            (kind == XL_SYM_UND && id >= o->nund))
            return 0;
        o->rel[i] = (XlReloc){off, (uint8_t)kind, id};
    }
    const char *s = in[S_NAMES].dst, *end = s + in[S_NAMES].len;
    for (uint32_t i = 0; i < o->nund; i++) {
        size_t n = strnlen(s, (size_t)(end - s));
        if (s + n >= end || !(o->und[i] = xl_alloc(n + 1))) return 0;
        memcpy(o->und[i], s, n + 1);
        s += n + 1;
    }
    xl_free(in[S_NAMES].dst);
    in[S_NAMES].dst = NULL;
    return 1;
}

void xl_unpack_drop(XlOut *o, XlpIn in[XLP_NSEC]) {
    xl_free(in[S_NAMES].dst);
    in[S_NAMES].dst = NULL;
    xl_free_out(o);
}

uint32_t xl_unpack_total(const XlpIn in[XLP_NSEC]) { return in[XLP_NSEC - 1].off + in[XLP_NSEC - 1].len; }

#define REC_RAW 0x80000000u

uint32_t xlp_records(const XlPack *p) { return (p->total + XLP_BLOCK - 1) / XLP_BLOCK; }
uint32_t xlp_store_max(const XlPack *p) { return p->total + 4 * xlp_records(p); }

// tload added the base to SECTION words and filled FUNC and UND words, which xlate leaves 0
static void unlink_into(const XlPack *p, const uint8_t *linked, uint32_t a, uint32_t m, uint8_t *d) {
    const uint32_t *rel = p->rel;
    uint32_t lo = 0, hi = p->sec[S_REL].len / 8;
    memcpy(d, linked + a, m);
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rel[2 * mid] < a) lo = mid + 1;
        else hi = mid;
    }
    for (uint32_t i = lo, n = p->sec[S_REL].len / 8; i < n && rel[2 * i] < a + m; i++) {
        uint32_t *w = (uint32_t *)(d + rel[2 * i] - a);
        *w = rel[2 * i + 1] >> 24 == XL_SYM_SECTION ? *w - (uint32_t)(uintptr_t)linked : 0;
    }
}

// The packed relocations, each offset as its distance from the one before: xlate emits them in order
static void delta_into(const uint32_t *rel, uint32_t a, uint32_t m, uint8_t *d) {
    uint32_t *w = (uint32_t *)d;
    for (uint32_t j = a / 4; j < (a + m) / 4; j++) *w++ = j & 1 ? rel[j] : rel[j] - (j ? rel[j - 2] : 0);
}

// The stored form's uncompressed bytes [off, off + n)
static void logical(const XlPack *p, uint32_t off, uint32_t n, uint8_t *out, const uint8_t *linked, uint32_t *tcrc) {
    if (off < XLP_HDR) memcpy(out, (const uint8_t *)p->hdr + off, XLP_HDR - off < n ? XLP_HDR - off : n);
    for (uint32_t i = 0; i < XLP_NSEC; i++) {
        const XlpOut *s = &p->sec[i];
        uint32_t lo = off > s->off ? off : s->off, hi = off + n < s->off + s->len ? off + n : s->off + s->len;
        if (lo >= hi) continue;
        uint8_t *d = out + (lo - off);
        if (i == S_TEXT && linked) unlink_into(p, linked, lo - s->off, hi - lo, d);
        else if (i == S_REL) delta_into(p->rel, lo - s->off, hi - lo, d);
        else memcpy(d, (const uint8_t *)s->src + (lo - s->off), hi - lo);
        if (i == S_TEXT && tcrc) *tcrc = xl_crc32(*tcrc, d, hi - lo);
    }
}

uint32_t xlp_record(const XlPack *p, uint32_t k, const uint8_t *linked, uint8_t *rec, uint8_t *scratch,
                    uint16_t *work, uint32_t *tcrc) {
    uint32_t off = k * XLP_BLOCK;
    if (off >= p->total) return 0;
    uint32_t n = p->total - off < XLP_BLOCK ? p->total - off : XLP_BLOCK, c, w;
    logical(p, off, n, scratch, linked, tcrc);
    if ((c = xlz_compress(scratch, n, rec + 4, work)) < n) {
        w = c;
    } else {
        memcpy(rec + 4, scratch, n);
        c = n, w = REC_RAW | n;
    }
    memcpy(rec, &w, 4);
    for (c += 4; c & 3; c++) rec[c] = 0;
    return c;
}

void xlp_load_begin(XlpLoad *l, uint32_t meta_crc, uint32_t meta_len, XlOut *o) {
    memset(o, 0, sizeof *o);
    memset(l->in, 0, sizeof l->in);
    l->o = o, l->meta_crc = meta_crc, l->meta_len = meta_len;
    l->pos = l->have = l->total = l->began = 0;
}

// One piece's uncompressed bytes into place: the header first, then each section's share
static int route(XlpLoad *l, const uint8_t *b, uint32_t n) {
    uint32_t off = l->pos;
    if (off < XLP_HDR) memcpy((uint8_t *)l->hdr + off, b, XLP_HDR - off < n ? XLP_HDR - off : n);
    l->pos += n;
    if (!l->began) {
        if (l->pos < XLP_HDR || !xl_unpack_begin(l->hdr, l->meta_crc, l->meta_len, l->o, l->in)) return 0;
        l->began = 1, l->total = xl_unpack_total(l->in);
    }
    if (l->pos > l->total) return 0;
    for (uint32_t i = 0; i < XLP_NSEC; i++) {
        const XlpIn *s = &l->in[i];
        uint32_t lo = off > s->off ? off : s->off, hi = off + n < s->off + s->len ? off + n : s->off + s->len;
        if (lo < hi) memcpy((uint8_t *)s->dst + (lo - s->off), b + (lo - off), hi - lo);
    }
    return 1;
}

int xlp_load_feed(XlpLoad *l, const uint8_t *b, uint32_t n) {
    while (n) {
        uint32_t need = 4, w = 0, len = 0;
        if (l->have >= 4) {
            memcpy(&w, l->rec, 4);
            len = w & ~REC_RAW;
            if (!len || len > (w & REC_RAW ? XLP_BLOCK : xlz_bound(XLP_BLOCK))) return 0;
            need = 4 + ((len + 3) & ~3u);
        }
        uint32_t k = need - l->have < n ? need - l->have : n;
        memcpy(l->rec + l->have, b, k);
        l->have += k, b += k, n -= k;
        if (l->have < need || need == 4) continue;
        l->have = 0;
        if (w & REC_RAW) {
            if (!route(l, l->rec + 4, len)) return 0;
        } else {
            int32_t d = xlz_decompress(l->rec + 4, len, l->blk, XLP_BLOCK);
            if (d <= 0 || !route(l, l->blk, (uint32_t)d)) return 0;
        }
    }
    return 1;
}

int xlp_load_end(XlpLoad *l) {
    if (!l->began || l->have || l->pos != l->total) return 0;
    uint32_t *pr = l->in[S_REL].dst;
    for (uint32_t i = 1; i < l->o->nrel; i++) pr[2 * i] += pr[2 * i - 2];
    return xl_unpack_end(l->o, l->in);
}

void xlp_load_drop(XlpLoad *l) { xl_unpack_drop(l->o, l->in); }
