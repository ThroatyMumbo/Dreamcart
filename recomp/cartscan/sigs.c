// The signature matcher: trials share each variable's {hi, lo} cell, so a failed nested trial can fill a half;
// failures are memoized only at j/jal targets, the one place a later query can run in a different context.
#include "sigs.h"

#include <stdlib.h>
#include <string.h>

#define OP_J 2
#define OP_JAL 3

typedef struct { uint8_t has_hi, has_lo; uint16_t hi, lo; } Cell;
typedef struct { uint32_t nf, nv, capf, capv; uint16_t *fname, *vname; uint32_t *faddr, *vcell; } Trial;

typedef struct {
    uint32_t nseg;
    struct { uint32_t lo, hi; int64_t d; } *seg;
    cs_page_fn page;
    void *ctx;
    uint32_t size;
    const uint32_t **pages;
    uint8_t *found_has, *var_has;
    uint32_t *found_addr;
    Cell *var;            // committed self.vars
    uint16_t *found_order, *var_order;
    uint32_t nfound, nvarorder;
    Cell *cells;          // trial cells for the current top-level attempt
    uint32_t ncells, capcells;
    uint32_t *targets;    // every j/jal target in the segments, sorted
    uint32_t ntargets, fwords;
    uint32_t *failed;     // per target, a bitset over symbols
    uint32_t *alias_addr;
    uint16_t *alias_name;
    uint32_t nalias, capalias;
    // The first four words at two addresses (a call target's check must not evict the caller's), and per mask
    // pattern the CRCs after 1..4 of them
    struct Pre { uint32_t addr, raw[4], gen; int n; uint32_t patgen[81], patcrc[81][4]; } pre[2];
    uint32_t gen;
    int prelast;
    uint8_t *vpat;                                 // per variant: base-3 mask pattern of words 0..3
    uint16_t *vref;                                // per variant and word 0..3: its xref's index in sig_refs
    uint32_t *tkey, *tval, tcap;                   // targets: address -> index, open addressed
    uint32_t *wlo, *whi, nwi, wlast;               // seg_of's intervals
    int *wseg;
    uint32_t novl;
    struct { uint32_t lo, hi, key; } ovl[CS_OV_SLOTS];  // overlays: run-time range, key base
    uint32_t taddr;
    int64_t tidx;
} M;
typedef struct Pre Pre;

static uint32_t crc_table[256];

uint32_t cs_crc32(uint32_t crc, const void *buf, size_t len) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    const uint8_t *p = buf;
    crc = ~crc;
    while (len--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static int rom_byte(M *m, int64_t off, uint8_t *b) {
    if (off < 0 || off >= m->size) return 0;
    uint32_t idx = (uint32_t)(off >> 2), pg = idx >> 10;
    if (!m->pages[pg]) m->pages[pg] = m->page(m->ctx, pg);
    *b = (uint8_t)(m->pages[pg][idx & 1023] >> (8 * (3 - (off & 3))));
    return 1;
}

// The first segment holding v, from a table of the intervals between segment edges
static int seg_of(M *m, uint32_t v) {
    uint32_t l = m->wlast;
    if (l < m->nwi && m->wlo[l] <= v && v < m->whi[l]) return m->wseg[l];
    uint32_t lo = 0, hi = m->nwi;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m->whi[mid] <= v) lo = mid + 1;
        else hi = mid;
    }
    if (lo < m->nwi && m->wlo[lo] <= v) return m->wseg[m->wlast = lo];
    return -1;
}

static void seg_table(M *m) {
    uint32_t *e = cs_alloc((2 * m->nseg + 1) * 4), ne = 0;
    for (uint32_t i = 0; i < m->nseg; i++) e[ne++] = m->seg[i].lo, e[ne++] = m->seg[i].hi;
    for (uint32_t i = 1; i < ne; i++) {
        uint32_t x = e[i], j = i;
        while (j > 0 && e[j - 1] > x) e[j] = e[j - 1], j--;
        e[j] = x;
    }
    m->wlo = cs_alloc((ne + 1) * 4), m->whi = cs_alloc((ne + 1) * 4), m->wseg = cs_alloc((ne + 1) * sizeof(int));
    m->nwi = 0;
    for (uint32_t i = 0; i + 1 < ne; i++) {
        if (e[i] == e[i + 1]) continue;
        int s = -1;
        for (uint32_t k = 0; k < m->nseg && s < 0; k++)
            if (m->seg[k].lo <= e[i] && e[i] < m->seg[k].hi) s = (int)k;
        m->wlo[m->nwi] = e[i], m->whi[m->nwi] = e[i + 1], m->wseg[m->nwi++] = s;
    }
    m->wlast = 0;
    cs_release(e);
}

static int word(M *m, uint32_t v, uint32_t *w) {
    int i = seg_of(m, v);
    if (i < 0) return 0;
    int64_t off = v + m->seg[i].d;
    if (off < 0 || off + 4 > m->size) return 0;
    if (!(off & 3)) {
        uint32_t idx = (uint32_t)(off >> 2), pg = idx >> 10;
        if (!m->pages[pg]) m->pages[pg] = m->page(m->ctx, pg);
        *w = m->pages[pg][idx & 1023];
        return 1;
    }
    uint8_t b[4];
    for (int k = 0; k < 4; k++)
        if (!rom_byte(m, off + k, &b[k])) return 0;
    *w = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
    return 1;
}

// A call target as the code at `at` sees it
static uint32_t mres(M *m, uint32_t at, uint32_t t) {
    if (!m->novl) return t;
    int s = seg_of(m, at);
    for (uint32_t i = 0; i < m->novl && s >= 0; i++)
        if (m->ovl[i].key == m->seg[s].lo && m->ovl[i].lo <= t && t < m->ovl[i].hi)
            return (m->ovl[i].key & ~CS_OV_MASK) | (t & CS_OV_MASK);
    if (CS_OV_IS_KEY(t)) return 1;
    uint32_t n = 0, one = 0;
    for (uint32_t i = 0; i < m->novl; i++)
        if (m->ovl[i].lo <= t && t < m->ovl[i].hi) n++, one = i;
    return n == 1 ? (m->ovl[one].key & ~CS_OV_MASK) | (t & CS_OV_MASK) : t;
}

static uint32_t thash(uint32_t a) { return (a >> 2) * 0x9E3779B1u; }

static int64_t target_idx(M *m, uint32_t addr) {
    if (m->tidx != -2 && m->taddr == addr) return m->tidx;
    m->taddr = addr, m->tidx = -1;
    for (uint32_t i = thash(addr) & (m->tcap - 1); m->tkey[i] != 0xFFFFFFFFu; i = (i + 1) & (m->tcap - 1))
        if (m->tkey[i] == addr) {
            m->tidx = m->tval[i];
            break;
        }
    return m->tidx;
}

static Pre *get_pre(M *m, uint32_t addr) {
    for (int i = 0; i < 2; i++)
        if (m->pre[i].n >= 0 && m->pre[i].addr == addr) return &m->pre[m->prelast = i];
    Pre *p = &m->pre[m->prelast = !m->prelast];
    p->addr = addr, p->n = 0, p->gen = ++m->gen;
    int s = seg_of(m, addr);
    if (s >= 0 && addr + 12 >= addr && addr + 12 < m->whi[m->wlast]) {  // one interval: four words in a row
        int64_t off = addr + m->seg[s].d;
        if (off >= 0 && !(off & 3) && off + 16 <= m->size) {
            for (int k = 0; k < 4; k++) {
                uint32_t idx = (uint32_t)(off >> 2) + k, pg = idx >> 10;
                if (!m->pages[pg]) m->pages[pg] = m->page(m->ctx, pg);
                p->raw[k] = m->pages[pg][idx & 1023];
            }
            p->n = 4;
            return p;
        }
    }
    for (int k = 0; k < 4 && word(m, addr + 4 * k, &p->raw[k]); k++) p->n = k + 1;
    return p;
}

// Elsewhere a failure is recomputed: at the top level a repeat query sees the same answer and has no side effects
static int failed_has(M *m, uint16_t name, uint32_t addr) {
    int64_t t = target_idx(m, addr);
    int si = sig_sym_of[name];
    return t >= 0 && si >= 0 && (m->failed[t * m->fwords + si / 32] >> (si % 32) & 1);
}

static void failed_add(M *m, uint16_t name, uint32_t addr) {
    int64_t t = target_idx(m, addr);
    int si = sig_sym_of[name];
    if (t >= 0 && si >= 0) m->failed[t * m->fwords + si / 32] |= 1u << (si % 32);
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void trial_free(Trial *t) {
    cs_release(t->fname), cs_release(t->faddr), cs_release(t->vname), cs_release(t->vcell);
    memset(t, 0, sizeof *t);
}

static void trial_copy(Trial *dst, const Trial *src) {
    dst->nf = dst->capf = src->nf + 1;
    dst->nv = src->nv;
    dst->capv = src->nv + 4;
    dst->fname = cs_alloc(dst->capf * 2), dst->faddr = cs_alloc(dst->capf * 4);
    dst->vname = cs_alloc(dst->capv * 2), dst->vcell = cs_alloc(dst->capv * 4);
    memcpy(dst->fname, src->fname, src->nf * 2), memcpy(dst->faddr, src->faddr, src->nf * 4);
    memcpy(dst->vname, src->vname, src->nv * 2), memcpy(dst->vcell, src->vcell, src->nv * 4);
    dst->nf = src->nf;
}

static void trial_addf(Trial *t, uint16_t name, uint32_t addr) {
    if (t->nf == t->capf) {
        t->capf = 2 * t->capf + 4;
        t->fname = cs_realloc(t->fname, t->capf * 2), t->faddr = cs_realloc(t->faddr, t->capf * 4);
    }
    t->fname[t->nf] = name, t->faddr[t->nf++] = addr;
}

static Cell *trial_cell(M *m, Trial *t, uint16_t var, int create) {
    for (uint32_t i = 0; i < t->nv; i++)
        if (t->vname[i] == var) return &m->cells[t->vcell[i]];
    if (!create) return NULL;
    if (m->ncells == m->capcells) {
        m->capcells = m->capcells ? 2 * m->capcells : 256;
        m->cells = cs_realloc(m->cells, m->capcells * sizeof *m->cells);
    }
    memset(&m->cells[m->ncells], 0, sizeof *m->cells);
    if (t->nv == t->capv) {
        t->capv = 2 * t->capv + 4;
        t->vname = cs_realloc(t->vname, t->capv * 2), t->vcell = cs_realloc(t->vcell, t->capv * 4);
    }
    t->vname[t->nv] = var, t->vcell[t->nv++] = m->ncells;
    return &m->cells[m->ncells++];
}

static int verify(M *m, uint16_t name, uint32_t addr, Trial *pend);

// zlib's CRC-32 over one word's little-endian bytes, four table lookups at once
static uint32_t crc_word(uint32_t c, uint32_t w) {
    static uint32_t t[4][256];
    if (!t[0][1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            t[0][i] = v;
        }
        for (uint32_t i = 0; i < 256; i++)
            for (int s = 1; s < 4; s++) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
    c = ~c ^ w;
    c = t[3][c & 0xFF] ^ t[2][(c >> 8) & 0xFF] ^ t[1][(c >> 16) & 0xFF] ^ t[0][c >> 24];
    return ~c;
}

static int in_pend(const Trial *pend, uint16_t var) {
    for (uint32_t i = 0; i < pend->nv; i++)
        if (pend->vname[i] == var) return 1;
    return 0;
}

// The first four words' masked CRC fails some symbols in any context, skipping the check if the xrefs before leave nothing
// (no nested verify or write to a cell shared with pend). 0: run the check; 1: fails before any xref; 2: after harmless ones
static int quick_reject(M *m, uint32_t addr, uint32_t vi, const Trial *pend, uint16_t name, int depth);

static uint8_t pat_digits[81][4];  // a mask pattern's base-3 digits per word: 0 none, 1 variable, 2 call

static void pat_init(void) {
    for (int p = 0; p < 81; p++)
        for (int k = 0, x = p; k < 4; k++, x /= 3) pat_digits[p][k] = (uint8_t)(x % 3);
}

static int in_funcs(const Trial *t, uint16_t f) {
    for (uint32_t i = 0; i < t->nf; i++)
        if (t->fname[i] == f) return 1;
    return 0;
}

// Would verify(tgt, target) inside this trial leave nothing behind? Yes when it returns before matching anything,
// or when every variant fails before reaching an xref (then it is not memoized either).
static int nested_inert(M *m, uint16_t tgt, uint32_t target, const Trial *pend, uint16_t name, int depth) {
    int si = sig_sym_of[tgt];
    if (si < 0 || m->found_has[tgt] || tgt == name || in_funcs(pend, tgt) || failed_has(m, tgt, target)) return 1;
    if (depth) return 0;
    const SigSym *s = &sig_syms[si];
    for (uint32_t k = 0; k < s->nvar; k++)
        if (quick_reject(m, target, s->var0 + k, pend, tgt, depth + 1) != 1) return 0;
    return 1;
}

static int quick_reject(M *m, uint32_t addr, uint32_t vi, const Trial *pend, uint16_t name, int depth) {
    const SigVar *v = &sig_vars[vi];
    Pre *P = get_pre(m, addr);
    uint32_t raw[4] = {P->raw[0], P->raw[1], P->raw[2], P->raw[3]};
    int n = P->n;
    uint32_t lim = v->numops < 4 ? v->numops : 4, pat = m->vpat[vi];
    if (!lim) return 0;
    uint32_t crc[4];
    if (P->patgen[pat] != P->gen) {
        uint32_t c = 0;
        for (int k = 0; k < n; k++) {
            uint32_t w = raw[k];
            if (pat_digits[pat][k] == 1) w &= ~0xFFFFu;
            else if (pat_digits[pat][k] == 2 || w >> 26 == OP_J) w &= ~0x03FFFFFFu;
            P->patcrc[pat][k] = c = crc_word(c, w);
        }
        P->patgen[pat] = P->gen;
    }
    memcpy(crc, P->patcrc[pat], sizeof crc);
    const uint8_t *dig = pat_digits[pat];
    int fk = -1, all_after = 0;  // the failing word, and whether its own xref ran first
    for (uint32_t k = 0; k < lim && fk < 0; k++) {
        if ((int)k >= n || (k == 0 && raw[0] >> 26 != v->firstop)) fk = k;
        else if (dig[k] == 2 && raw[k] >> 26 != OP_J && raw[k] >> 26 != OP_JAL) fk = k;
        else if (k == 3 && crc[3] != v->partial) fk = 3, all_after = 1;
    }
    if (fk < 0) {
        if (lim != v->numops || crc[lim - 1] == v->crc) return 0;
        fk = lim - 1, all_after = 1;
    }
    int reached = 0, upto = all_after ? fk : fk - 1;
    for (int k = 0; k <= upto; k++) {
        uint32_t d = dig[k];
        if (!d) continue;
        const SigRef *r = &sig_refs[m->vref[4 * vi + k]];
        if (d == 2) {
            uint32_t target = mres(m, addr + 4 * k, ((addr + 4 * k) & 0xF0000000u) | ((raw[k] & 0x03FFFFFFu) << 2));
            if (!nested_inert(m, r->target, target, pend, name, depth)) return 0;
        } else if (in_pend(pend, r->target)) {
            return 0;
        }
        reached = 1;
    }
    return reached ? 2 : 1;
}

static int check(M *m, uint32_t addr, const SigVar *v, Trial *trial) {
    uint32_t c = 0;
    const SigRef *ref = &sig_refs[v->ref0], *rend = ref + v->nref;
    for (uint32_t k = 0; k < v->numops; k++) {
        uint32_t w;
        if (!word(m, addr + 4 * k, &w) || (k == 0 && w >> 26 != v->firstop)) return 0;
        if (ref < rend && ref->off == k) {
            if (ref->kind == SR_FUNCTION) {
                if (w >> 26 != OP_J && w >> 26 != OP_JAL) return 0;
                uint32_t target = mres(m, addr + 4 * k, ((addr + 4 * k) & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2));
                if (sig_sym_of[ref->target] >= 0 && !verify(m, ref->target, target, trial)) return 0;
                w &= ~0x03FFFFFFu;
            } else {
                int hi = ref->kind == SR_VAR_HI;
                const Cell *com = m->var_has[ref->target] ? &m->var[ref->target] : NULL;
                int have = 0;
                uint16_t bound = 0;
                if (com && (hi ? com->has_hi : com->has_lo)) have = 1, bound = hi ? com->hi : com->lo;
                else {
                    const Cell *tc = trial_cell(m, trial, ref->target, 0);
                    if (tc && (hi ? tc->has_hi : tc->has_lo)) have = 1, bound = hi ? tc->hi : tc->lo;
                }
                if (have && bound != (w & 0xFFFF)) return 0;
                Cell *cell = trial_cell(m, trial, ref->target, 1);
                if (hi) cell->has_hi = 1, cell->hi = w & 0xFFFF;
                else cell->has_lo = 1, cell->lo = w & 0xFFFF;
                w &= ~0xFFFFu;
            }
            ref++;
        } else if (w >> 26 == OP_J) {
            w &= ~0x03FFFFFFu;
        }
        c = crc_word(c, w);
        if (k == 3 && c != v->partial) return 0;
    }
    return c == v->crc;
}

static int verify(M *m, uint16_t name, uint32_t addr, Trial *pend) {
    if (m->found_has[name]) return m->found_addr[name] == addr;
    for (uint32_t i = 0; i < pend->nf; i++)
        if (pend->fname[i] == name) return pend->faddr[i] == addr;
    int si = sig_sym_of[name];
    if (si < 0 || failed_has(m, name, addr)) return 0;
    const SigSym *s = &sig_syms[si];
    int pure = 1;
    for (uint32_t k = 0; k < s->nvar; k++) {
        int q = quick_reject(m, addr, s->var0 + k, pend, name, 0);
        if (q == 1) continue;
        pure = 0;
        if (q == 2) continue;
        Trial t;
        trial_copy(&t, pend);
        trial_addf(&t, name, addr);
        if (check(m, addr, &sig_vars[s->var0 + k], &t)) {
            trial_free(pend);
            *pend = t;
            return 1;
        }
        trial_free(&t);
    }
    if (!pure) failed_add(m, name, addr);
    return 0;
}

// Away from j/jal targets a simple symbol (4+ words, no call xref) can only match where a variant's masked CRC equals its
// partial CRC; elsewhere it fails unmemoized. Mask CRCs follow by linearity: crc(m ^ d) = crc(m) ^ L(d), 16-byte messages.
typedef struct {
    uint8_t *simple;
    uint32_t gstart[65];
    uint8_t *gpat;
    uint32_t *gfirst, *gend;
    uint32_t *partial;
    uint16_t *sym;
    uint32_t L[16][256];
    uint16_t *touched;
    uint32_t ntouched;
    // Per opcode, the symbols that aren't simple: whether each variant starting with that opcode has a call xref in
    // its first four words (ok), and those first call slots
    uint32_t nsc_start[65];
    uint16_t *nsc_sym;
    uint8_t *nsc_ok;
    uint32_t *nsc_kf0;
    uint8_t *nsc_kf;
} Fast;

typedef struct { uint32_t op, pat, partial, sym; } FastEnt;

static int cmp_fast(const void *a, const void *b) {
    const FastEnt *x = a, *y = b;
    if (x->op != y->op) return x->op < y->op ? -1 : 1;
    if (x->pat != y->pat) return x->pat < y->pat ? -1 : 1;
    if (x->partial != y->partial) return x->partial < y->partial ? -1 : 1;
    return x->sym < y->sym ? -1 : x->sym > y->sym;
}

static void fast_init(Fast *F, const uint8_t *vpat, uint32_t nv) {
    memset(F, 0, sizeof *F);
    F->simple = cs_alloc(sig_nsyms + 1);
    F->touched = cs_alloc((sig_nsyms + 1) * 2);
    FastEnt *e = cs_alloc((nv + 1) * sizeof *e);
    uint32_t ne = 0;
    for (uint32_t si = 0; si < sig_nsyms; si++) {
        const SigSym *s = &sig_syms[si];
        int simple = 1;
        for (uint32_t k = 0; k < s->nvar && simple; k++) {
            const SigVar *v = &sig_vars[s->var0 + k];
            simple = v->numops >= 4;
            for (uint32_t r = 0; r < v->nref && simple; r++)
                simple = !(sig_refs[v->ref0 + r].off < 4 && sig_refs[v->ref0 + r].kind == SR_FUNCTION);
        }
        F->simple[si] = (uint8_t)simple;
        if (simple)
            for (uint32_t k = 0; k < s->nvar; k++)
                e[ne++] = (FastEnt){sig_vars[s->var0 + k].firstop, vpat[s->var0 + k], sig_vars[s->var0 + k].partial, si};
    }
    qsort(e, ne, sizeof *e, cmp_fast);
    F->gpat = cs_alloc(ne + 1), F->gfirst = cs_alloc((ne + 1) * 4), F->gend = cs_alloc((ne + 1) * 4);
    F->partial = cs_alloc((ne + 1) * 4), F->sym = cs_alloc((ne + 1) * 2);
    uint32_t ng = 0;
    for (uint32_t op = 0, i = 0; op < 64; op++) {
        F->gstart[op] = ng;
        while (i < ne && e[i].op == op) {
            F->gpat[ng] = (uint8_t)e[i].pat, F->gfirst[ng] = i;
            while (i < ne && e[i].op == op && e[i].pat == F->gpat[ng]) F->partial[i] = e[i].partial, F->sym[i] = (uint16_t)e[i].sym, i++;
            F->gend[ng++] = i;
        }
    }
    F->gstart[64] = ng;
    cs_release(e);
    uint32_t cap = sig_first_start[64] + 1, nns = 0, nkf = 0;
    F->nsc_sym = cs_alloc(cap * 2), F->nsc_ok = cs_alloc(cap), F->nsc_kf0 = cs_alloc((cap + 1) * 4);
    F->nsc_kf = cs_alloc(nv + 1);
    for (uint32_t op = 0; op < 64; op++) {
        F->nsc_start[op] = nns;
        for (uint32_t k = sig_first_start[op]; k < sig_first_start[op + 1]; k++) {
            uint16_t si = sig_first_list[k];
            int dup = F->simple[si];
            for (uint32_t j = F->nsc_start[op]; j < nns && !dup; j++) dup = F->nsc_sym[j] == si;
            if (dup) continue;
            const SigSym *s = &sig_syms[si];
            F->nsc_sym[nns] = si, F->nsc_ok[nns] = 1, F->nsc_kf0[nns] = nkf;
            for (uint32_t vi = 0; vi < s->nvar; vi++) {
                const SigVar *v = &sig_vars[s->var0 + vi];
                if (v->firstop != op) continue;
                int kf = -1;
                for (uint32_t r = 0; r < v->nref && kf < 0; r++)
                    if (sig_refs[v->ref0 + r].kind == SR_FUNCTION) kf = sig_refs[v->ref0 + r].off;
                if (v->numops < 4 || kf < 0 || kf > 3) F->nsc_ok[nns] = 0;
                else F->nsc_kf[nkf++] = (uint8_t)kf;
            }
            nns++;
        }
    }
    F->nsc_start[64] = nns, F->nsc_kf0[nns] = nkf;
    uint8_t msg[16] = {0};
    uint32_t zero = cs_crc32(0, msg, 16);
    for (int pos = 0; pos < 16; pos++)
        for (int v = 0; v < 256; v++) {
            msg[pos] = (uint8_t)v;
            F->L[pos][v] = cs_crc32(0, msg, 16) ^ zero;
            msg[pos] = 0;
        }
}

// Marks the simple symbols that may act at addr; 0 when the filter can't decide (fewer than four words)
static int fast_hits(Fast *F, M *m, uint32_t addr, uint32_t op, uint8_t *hit) {
    Pre *P = get_pre(m, addr);
    if (P->n < 4) return 0;
    uint32_t w0[4], c0 = 0;
    for (int k = 0; k < 4; k++) {
        uint32_t w = P->raw[k];
        w0[k] = w >> 26 == OP_J ? w & ~0x03FFFFFFu : w;
        c0 = crc_word(c0, w0[k]);
    }
    // A call-xref symbol fails at its first call slot, before any nested verify, where that word isn't j/jal
    for (uint32_t i = F->nsc_start[op]; i < F->nsc_start[op + 1]; i++) {
        int act = !F->nsc_ok[i];
        for (uint32_t j = F->nsc_kf0[i]; j < F->nsc_kf0[i + 1] && !act; j++)
            act = P->raw[F->nsc_kf[j]] >> 26 == OP_J || P->raw[F->nsc_kf[j]] >> 26 == OP_JAL;
        uint16_t s = F->nsc_sym[i];
        if (act && !hit[s]) hit[s] = 1, F->touched[F->ntouched++] = s;
    }
    for (uint32_t g = F->gstart[op]; g < F->gstart[op + 1]; g++) {
        uint32_t c = c0;
        const uint8_t *dig = pat_digits[F->gpat[g]];
        for (int k = 0; k < 4; k++) {
            if (!dig[k]) continue;
            uint32_t w = P->raw[k], d = w0[k] ^ (dig[k] == 1 ? w & ~0xFFFFu : w & ~0x03FFFFFFu);
            for (int b = 0; b < 4; b++, d >>= 8)
                if (d & 0xFF) c ^= F->L[4 * k + b][d & 0xFF];
        }
        uint32_t lo = F->gfirst[g], hi = F->gend[g];
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (F->partial[mid] < c) lo = mid + 1;
            else hi = mid;
        }
        for (; lo < F->gend[g] && F->partial[lo] == c; lo++)
            if (!hit[F->sym[lo]]) hit[F->sym[lo]] = 1, F->touched[F->ntouched++] = F->sym[lo];
    }
    return 1;
}

static void fast_clear(Fast *F, uint8_t *hit) {
    while (F->ntouched) hit[F->touched[--F->ntouched]] = 0;
}

static void fast_free(Fast *F) {
    cs_release(F->simple), cs_release(F->touched), cs_release(F->gpat), cs_release(F->gfirst);
    cs_release(F->gend), cs_release(F->partial), cs_release(F->sym);
    cs_release(F->nsc_sym), cs_release(F->nsc_ok), cs_release(F->nsc_kf0), cs_release(F->nsc_kf);
}

static void push16(uint16_t **a, uint32_t n, uint16_t x) {
    if (!(n & (n - 1))) *a = cs_realloc(*a, (n ? 2 * n : 1) * 2);
    (*a)[n] = x;
}
static void push32(uint32_t **a, uint32_t n, uint32_t x) {
    if (!(n & (n - 1))) *a = cs_realloc(*a, (n ? 2 * n : 1) * 4);
    (*a)[n] = x;
}

static int alias_has(const M *m, uint32_t addr) {
    for (uint32_t i = 0; i < m->nalias; i++)
        if (m->alias_addr[i] == addr) return 1;
    return 0;
}

static void sort_pairs(uint32_t *key, uint16_t *val, uint32_t n) {  // stable insertion sort by key
    for (uint32_t i = 1; i < n; i++) {
        uint32_t k = key[i];
        uint16_t v = val[i];
        uint32_t j = i;
        while (j > 0 && key[j - 1] > k) key[j] = key[j - 1], val[j] = val[j - 1], j--;
        key[j] = k, val[j] = v;
    }
}

static int64_t var_value(const HmResult *r, const char *name) {
    for (uint32_t i = 0; i < r->nvar; i++)
        if (!strcmp(sig_names[r->var_name[i]], name)) return r->var_addr[i];
    return -1;
}

HmResult *hm_run(const CsResult *scan, cs_page_fn page, void *ctx, uint32_t rom_size) {
    return hm_run_seg(scan, -1, page, ctx, rom_size);
}

// The matcher's state for one run: variant reference patterns, segments, overlays and the result tables; returns
// the variant count
static uint32_t m_init(M *m, const CsResult *scan, cs_page_fn page, void *ctx, uint32_t rom_size) {
    memset(m, 0, sizeof *m);
    pat_init();
    m->pre[0].n = m->pre[1].n = -1, m->tidx = -2, m->gen = 1;
    uint32_t nv = 0;
    for (uint32_t i = 0; i < sig_nsyms; i++)
        if (sig_syms[i].var0 + sig_syms[i].nvar > nv) nv = sig_syms[i].var0 + sig_syms[i].nvar;
    m->vpat = cs_alloc(nv + 1);
    m->vref = cs_alloc((4 * nv + 4) * 2);
    for (uint32_t i = 0; i < nv; i++) {
        uint32_t pat = 0, pw[4] = {1, 3, 9, 27};
        for (uint32_t k = 0; k < sig_vars[i].nref; k++) {
            const SigRef *r = &sig_refs[sig_vars[i].ref0 + k];
            if (r->off < 4) pat += pw[r->off] * (r->kind == SR_FUNCTION ? 2 : 1), m->vref[4 * i + r->off] = sig_vars[i].ref0 + k;
        }
        m->vpat[i] = (uint8_t)pat;
    }
    m->page = page, m->ctx = ctx, m->size = rom_size;
    m->pages = cs_alloc(((rom_size + 4095) / 4096 + 1) * sizeof *m->pages);
    memset(m->pages, 0, ((rom_size + 4095) / 4096 + 1) * sizeof *m->pages);
    m->nseg = scan->nseg;
    m->seg = cs_alloc((scan->nseg + 1) * sizeof *m->seg);
    for (uint32_t i = 0; i < scan->nseg; i++)
        m->seg[i].lo = scan->seg[i].vram, m->seg[i].hi = scan->seg[i].vram + scan->seg[i].size,
        m->seg[i].d = (int64_t)scan->seg[i].rom - scan->seg[i].vram;
    for (uint32_t i = 0; i < scan->nseg; i++) {
        uint32_t v = scan->seg[i].vram;
        if (v < CS_OV_BASE || m->novl == CS_OV_SLOTS) continue;
        uint32_t r = 0x80000000u | (v & CS_OV_MASK);
        m->ovl[m->novl].lo = r, m->ovl[m->novl].hi = r + scan->seg[i].size, m->ovl[m->novl++].key = v;
    }
    seg_table(m);
    uint32_t nn = sig_nnames;
    m->found_has = cs_alloc(nn), m->var_has = cs_alloc(nn);
    memset(m->found_has, 0, nn), memset(m->var_has, 0, nn);
    m->found_addr = cs_alloc(nn * 4), m->var = cs_alloc(nn * sizeof *m->var);
    memset(m->var, 0, nn * sizeof *m->var);
    m->found_order = cs_alloc(nn * 2), m->var_order = cs_alloc(nn * 2);
    return nv;
}

// Every j/jal target in the scanned segments, sorted and hashed, with a failed-signature bitmap per target
static void collect_target(M *m, uint32_t addr, uint32_t *cap) {
    uint32_t w;
    if (!word(m, addr, &w) || (w >> 26 != OP_J && w >> 26 != OP_JAL)) return;
    if (m->ntargets == *cap) m->targets = cs_realloc(m->targets, (*cap *= 2) * 4);
    m->targets[m->ntargets++] = mres(m, addr, (addr & 0xF0000000u) | ((w & 0x03FFFFFFu) << 2));
}

static void collect_targets(M *m, const CsResult *scan, int only) {
    uint32_t cap = 1024;
    m->targets = cs_alloc(cap * 4);
    if (only >= 0) {  // a run-time region: its functions' words only
        for (uint32_t i = 0; i < scan->nfunc; i++)
            for (uint32_t addr = scan->func[i].start; addr < scan->func[i].end; addr += 4)
                if (addr - m->seg[only].lo < m->seg[only].hi - m->seg[only].lo) collect_target(m, addr, &cap);
    } else
        for (uint32_t si = 0; si < m->nseg; si++)
            for (uint32_t addr = m->seg[si].lo; addr < m->seg[si].hi; addr += 4) collect_target(m, addr, &cap);
    qsort(m->targets, m->ntargets, 4, cmp_u32);
    uint32_t nt = 0;
    for (uint32_t i = 0; i < m->ntargets; i++)
        if (!nt || m->targets[i] != m->targets[nt - 1]) m->targets[nt++] = m->targets[i];
    m->ntargets = nt;
    for (m->tcap = 16; m->tcap < 2 * nt; m->tcap *= 2) {}
    m->tkey = cs_alloc(m->tcap * 4), m->tval = cs_alloc(m->tcap * 4);
    memset(m->tkey, 0xFF, m->tcap * 4);
    for (uint32_t i = 0; i < nt; i++) {
        uint32_t h = thash(m->targets[i]) & (m->tcap - 1);
        while (m->tkey[h] != 0xFFFFFFFFu) h = (h + 1) & (m->tcap - 1);
        m->tkey[h] = m->targets[i], m->tval[h] = i;
    }
    m->fwords = (sig_nsyms + 31) / 32;
    m->failed = cs_alloc(((size_t)nt * m->fwords + 1) * 4);
    memset(m->failed, 0, ((size_t)nt * m->fwords + 1) * 4);
}

// A leaf already found elsewhere that also matches at addr: a second copy of it, recorded as an alias
__attribute__((noinline)) static void try_alias(M *m, const SigSym *s, uint32_t addr) {
    uint16_t name = s->name;
    if (!s->leaf || m->found_addr[name] == addr || alias_has(m, addr)) return;
    int any = 0;
    for (uint32_t vi = 0; vi < s->nvar && !any; vi++) {
        Trial t = {0};
        if (sig_vars[s->var0 + vi].nref) continue;  // aliases come from leaf variants only
        if (quick_reject(m, addr, s->var0 + vi, &t, name, 0)) continue;
        m->ncells = 0;
        any = check(m, addr, &sig_vars[s->var0 + vi], &t);
        trial_free(&t);
    }
    if (any) {
        push32(&m->alias_addr, m->nalias, addr);
        push16(&m->alias_name, m->nalias, name);
        m->nalias++;
    }
}

// A verified match: its functions and the variables its cells resolved
__attribute__((noinline)) static void commit_trial(M *m, const Trial *pend) {
    for (uint32_t i = 0; i < pend->nf; i++)
        if (!m->found_has[pend->fname[i]]) {
            m->found_has[pend->fname[i]] = 1;
            m->found_order[m->nfound++] = pend->fname[i];
        }
    for (uint32_t i = 0; i < pend->nf; i++) m->found_addr[pend->fname[i]] = pend->faddr[i];
    for (uint32_t i = 0; i < pend->nv; i++) {
        uint16_t var = pend->vname[i];
        const Cell *c = &m->cells[pend->vcell[i]];
        if (!m->var_has[var]) m->var_has[var] = 1, m->var_order[m->nvarorder++] = var;
        if (c->has_hi) m->var[var].has_hi = 1, m->var[var].hi = c->hi;
        if (c->has_lo) m->var[var].has_lo = 1, m->var[var].lo = c->lo;
    }
}

// Every signature whose first word can be the one at addr
static void match_at(M *m, Fast *fast, uint8_t *hit, uint32_t addr) {
    uint32_t w;
    if (!word(m, addr, &w)) return;
    uint32_t op = w >> 26;
    if (sig_first_start[op] == sig_first_start[op + 1]) return;
    int filtered = target_idx(m, addr) < 0 && fast_hits(fast, m, addr, op, hit);
    if (filtered && !fast->ntouched) return;  // nothing here can act
    for (uint32_t k = sig_first_start[op]; k < sig_first_start[op + 1]; k++) {
        if (filtered && !hit[sig_first_list[k]]) continue;
        const SigSym *s = &sig_syms[sig_first_list[k]];
        if (m->found_has[s->name]) {
            try_alias(m, s, addr);
            continue;
        }
        Trial pend = {0};
        m->ncells = 0;
        if (verify(m, s->name, addr, &pend)) commit_trial(m, &pend);
        trial_free(&pend);
    }
    if (filtered) fast_clear(fast, hit);
}

static HmResult *hm_result(const M *m);
static void os_symbols(HmResult *r);

HmResult *hm_run_seg(const CsResult *scan, int only, cs_page_fn page, void *ctx, uint32_t rom_size) {
    static M m;
    uint32_t nv = m_init(&m, scan, page, ctx, rom_size);
    collect_targets(&m, scan, only);
    Fast fast;
    fast_init(&fast, m.vpat, nv);
    uint8_t *hit = cs_alloc(sig_nsyms + 1);
    memset(hit, 0, sig_nsyms + 1);
    if (only >= 0) {  // a run-time region (rgn.c): its scanned starts, not each word of the data around its code
        for (uint32_t i = 0; i < scan->nfunc; i++)
            if (scan->func[i].start - m.seg[only].lo < m.seg[only].hi - m.seg[only].lo)
                match_at(&m, &fast, hit, scan->func[i].start);
    } else
        for (uint32_t si = 0; si < m.nseg; si++)
            for (uint32_t addr = m.seg[si].lo; addr < m.seg[si].hi; addr += 4) match_at(&m, &fast, hit, addr);
    fast_free(&fast);
    cs_release(hit);
    HmResult *r = hm_result(&m);
    os_symbols(r);
    cs_release(m.pages), cs_release(m.seg), cs_release(m.found_has), cs_release(m.var_has);
    cs_release(m.found_addr), cs_release(m.var), cs_release(m.found_order), cs_release(m.var_order);
    cs_release(m.cells), cs_release(m.failed), cs_release(m.targets), cs_release(m.alias_addr), cs_release(m.alias_name);
    cs_release(m.vpat), cs_release(m.vref), cs_release(m.tkey), cs_release(m.tval);
    cs_release(m.wlo), cs_release(m.whi), cs_release(m.wseg);
    return r;
}

// The functions and variables found, and every address's name
static HmResult *hm_result(const M *m) {
    HmResult *r = cs_alloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->nfunc = m->nfound;
    r->func_name = cs_alloc((m->nfound + 1) * 2), r->func_addr = cs_alloc((m->nfound + 1) * 4);
    for (uint32_t i = 0; i < m->nfound; i++)
        r->func_name[i] = m->found_order[i], r->func_addr[i] = m->found_addr[m->found_order[i]];
    r->var_name = cs_alloc((m->nvarorder + 1) * 2), r->var_addr = cs_alloc((m->nvarorder + 1) * 4);
    for (uint32_t i = 0; i < m->nvarorder; i++) {
        const Cell *c = &m->var[m->var_order[i]];
        if (!c->has_hi || !c->has_lo) continue;
        r->var_name[r->nvar] = m->var_order[i];
        r->var_addr[r->nvar++] = ((uint32_t)c->hi << 16) + (uint32_t)(int32_t)(int16_t)c->lo;
    }
    // by_address: the last function identified at an address names it, then leaf aliases fill the rest
    r->addr = cs_alloc((m->nfound + m->nalias + 1) * 4), r->addr_name = cs_alloc((m->nfound + m->nalias + 1) * 2);
    for (uint32_t i = 0; i < m->nfound; i++) {
        uint32_t a = r->func_addr[i], j;
        for (j = 0; j < r->naddr && r->addr[j] != a; j++) {}
        if (j == r->naddr) r->addr[r->naddr++] = a;
        r->addr_name[j] = r->func_name[i];
    }
    uint32_t nfa = r->naddr;
    for (uint32_t i = 0; i < m->nalias; i++) {
        uint32_t j;
        for (j = 0; j < nfa && r->addr[j] != m->alias_addr[i]; j++) {}
        if (j < nfa) continue;
        for (j = nfa; j < r->naddr && r->addr[j] != m->alias_addr[i]; j++) {}
        if (j == r->naddr) r->addr[r->naddr++] = m->alias_addr[i];
        r->addr_name[j] = m->alias_name[i];
    }
    sort_pairs(r->addr, r->addr_name, r->naddr);
    return r;
}

// The libultra thread, event and RCP tables os.c needs, from whichever of their names the signatures found
static void os_symbols(HmResult *r) {
    static const struct { const char *name; uint32_t off; } tail[] = {
        {"osNullMsgQueue", 0}, {"__osThreadTail_8", 8}, {"__osThreadTail_10", 0x10}};
    static const char *const evt[] = {"__osEventStateTab_0", "osEventMesgArray"};
    static const char *const im[] = {"__osRcpImTable_0", "osDispatchThreadRCPThingamy"};
    int64_t v = -1;
    for (int i = 0; i < 3 && v < 0; i++)
        if ((v = var_value(r, tail[i].name)) >= 0) r->os_thread_tail = (uint32_t)v - tail[i].off;
    if (v < 0) r->os_missing = "__osThreadTail";
    v = -1;
    for (int i = 0; i < 2 && v < 0; i++)
        if ((v = var_value(r, evt[i])) >= 0) r->os_event_tab = (uint32_t)v;
    if (v < 0 && !r->os_missing) r->os_missing = "__osEventStateTab";
    v = -1;
    for (int i = 0; i < 2 && v < 0; i++)
        if ((v = var_value(r, im[i])) >= 0) r->os_rcp_im_table = (uint32_t)v;
    if (v < 0 && !r->os_missing) r->os_missing = "__osRcpImTable";
    for (uint32_t i = 0; i < r->nfunc; i++)
        if (!strcmp(sig_names[r->func_name[i]], "osDestroyThread")) r->os_destroy_thread = r->func_addr[i];
}

void hm_free(HmResult *r) {
    if (!r) return;
    cs_release(r->func_name), cs_release(r->func_addr), cs_release(r->var_name), cs_release(r->var_addr);
    cs_release(r->addr), cs_release(r->addr_name);
    cs_release(r);
}
