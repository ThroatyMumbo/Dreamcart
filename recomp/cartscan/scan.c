#include "scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cs_vec.h"
#include "instructions/RabbitizerInstruction.h"
#include "common/RabbitizerConfig.h"

#define JR_RA 0x03E00008u
#define ERET 0x42000018u
#define B_NEXT 0x10000001u
#define SELF_LOOP 0x1000FFFFu
#define BOOT_ROM 0x1000u
#define BOOT_SIZE 0x100000u
#define SLACK 0x40000u
#define MAX_PAD 8
#define JAL_OP 0x03u
#define CACHE_OP 0x2Fu
#define HIT_INV_I 0x10u
#define OV_BASE CS_OV_BASE
#define OV_SPAN CS_OV_SPAN
#define OV_END CS_OV_END
#define OV_MAX CS_OV_ROM_MAX
#define OV_LEN_MAX 0x400000u
#define OV_RDRAM_LO 0x80000400u
#define OV_RDRAM_HI 0x80400000u

// Scanner state
typedef struct { uint32_t vram, lo, hi, how, how_n; i64 delta; u64 limit; uint32_t real; } Seg;  // real: an overlay's run-time vram, else 0
typedef struct { uint32_t jr, table; Vec targets; } Tab;

typedef struct {
    uint32_t start, end, maxseen, nseen;
    Vec calls, tails, unres, absorbed;
    Tab *tabs;
    uint32_t ntabs, captabs;
    uint8_t *seen;
    uint32_t seencap;  // bits
    uint32_t memo_gen; // the memo entry this is an unmodified copy of, or 0
    uint32_t refs;     // the function table and the explore memo share one object
} Func;

// An explore result and what it read: the 256-byte blocks whose start/strong flags it tested, and whether it read
// jump-table words (segments) or ran an unbounded table scan (table_entries, never reused)
typedef struct {
    Func *f;
    uint32_t badpc, stamp, segver, gen, novl;
    uint8_t bad, words, unbounded, valid;
    Vec deps;
    uint32_t ic_gen, ic_epoch;  // icache_segments' view of f: valid while the window winners stay put
    uint8_t ic_inval;
    Vec ic_calls;
    uint32_t pv_gen;            // pointer_seeds' lui/addiu values in f
    Vec pv;
} Memo;

enum { C_OTHER, C_JR, C_JAL, C_JALR, C_J, C_LUI, C_ADDIU, C_ORI, C_LW, C_ADDU, C_SLL, C_SLTIU };
#define I_PROBED 0x8000
#define I_NONE 0x4000
#define I_VALID 0x0001
#define I_DELAY 0x0002
#define I_JRRA 0x0004
#define I_LINK 0x0008
#define I_MODRT 0x0010
#define I_MODRD 0x0020
#define I_LABEL 0x0040
#define I_CLASS(i) (((i) >> 8) & 15)

typedef struct { uint16_t info[1024]; uint8_t seg[1024]; } Page;  // seg: the window the word was read through
typedef struct { uint32_t raw; uint16_t info; } Ins;

struct Scan {
    uint32_t size, nwords, entry;
    cs_page_fn page;
    void *ctx;
    const uint32_t **rom_pages;
    Seg *seg;
    uint32_t nseg, capseg;
    uint32_t ovl[OV_MAX], novl;  // the overlay segments, in the order found
    uint32_t segver;     // bumped on every segment change; segment_of caches against it
    uint32_t map_epoch;  // bumped when a rebuilt window table gives some covered address a different winner
    struct { uint32_t epoch; i64 lo, hi; } *moved;  // which ranges changed winner, by epoch
    uint32_t nmoved, capmoved;
    struct { uint32_t ver, n, last; i64 lo[64], hi[64]; int seg[64]; } wc;
    Map funcs;  // start -> index into fpool
    Func **fpool;
    uint32_t nfpool, capfpool;
    Vec ffree, sorted;
    Bits starts, strong, table_entries;
    Map bad, dead_end, origin, unplaced, unplaceable, tried;
    Map cache_pages;  // vram >> 12 -> index into pages
    Page **pages;
    uint32_t npages, cappages, last_cpage_key;
    Page *last_cpage;
    Map rom_starts_set;
    Vec rom_starts, ptr_words;
    uint32_t fed;
    struct { uint32_t p, steps; } pend[16];
    uint32_t npend;
    Map tt;
    i64 tt_count;
    uint32_t epoch, memo_gen;
    Map blk_pages, memo_of;  // vram >> 12 -> 16 block epochs; start -> memo
    uint32_t (*blk)[16];
    uint32_t nblk, capblk;
    Memo *memo;
    uint32_t nmemo, capmemo;
    Vec *deps;       // the explore in progress records its blocks here
    uint8_t dep_words, dep_unbounded, dropped, ptr_ranged;
    i64 ptr_lo, ptr_hi;
    CsResult res;
};

// ptr_words is consulted over [lo, hi) only: with a ranged table, that span must lie inside its range
static void ptr_span(Scan *S, i64 lo, i64 hi) {
    if (S->ptr_ranged && lo < hi && (lo < S->ptr_lo || hi > S->ptr_hi)) S->res.tables_short = 1;
}

static void blk_bump(Scan *S, uint32_t a) {
    i64 i = map_get(&S->blk_pages, a >> 12);
    if (i == NONE) {
        if (S->nblk == S->capblk) {
            S->capblk = S->capblk ? 2 * S->capblk : 64;
            S->blk = cs_realloc(S->blk, S->capblk * sizeof *S->blk);
        }
        memset(S->blk[S->nblk], 0, sizeof *S->blk);
        i = S->nblk++;
        map_put(&S->blk_pages, a >> 12, (uint32_t)i);
    }
    S->blk[i][(a >> 8) & 15] = ++S->epoch;
}

static uint32_t blk_epoch(Scan *S, uint32_t block) {
    i64 i = map_get(&S->blk_pages, block >> 4);
    return i == NONE ? 0 : S->blk[i][block & 15];
}

static void dep(Scan *S, uint32_t a) {
    if (S->deps && (!S->deps->n || S->deps->v[S->deps->n - 1] != a >> 8)) vpush(S->deps, a >> 8);
}

// starts and strong, which explore reads, mark the block of every change
static void mark_add(Scan *S, Bits *b, uint32_t a) {
    if (bits_add(b, a)) blk_bump(S, a);
}
static void mark_del(Scan *S, Bits *b, uint32_t a) {
    if (bits_del(b, a)) blk_bump(S, a);
}
static void dead_put(Scan *S, uint32_t start, uint32_t end) {
    if (map_get(&S->dead_end, start) != end) map_put(&S->dead_end, start, end), blk_bump(S, start);
}

static uint32_t rom_word_at(Scan *S, uint32_t idx) {
    uint32_t pg = idx >> 10;
    if (!S->rom_pages[pg]) S->rom_pages[pg] = S->page(S->ctx, pg);
    return S->rom_pages[pg][idx & 1023];
}

static i64 rom_word(Scan *S, i64 off) {
    if (off < 0 || off >= (i64)S->size - 3) return NONE;
    return rom_word_at(S, (uint32_t)(off >> 2));
}

static void window(const Seg *s, i64 *lo, i64 *hi) {
    if (s->limit) *lo = s->vram, *hi = (i64)s->limit;
    else *lo = s->lo, *hi = (i64)s->hi + SLACK;
}

static int segment_of_slow(const Scan *S, i64 v) {
    int best = -1;
    i64 best_lo = -1;
    for (uint32_t i = 0; i < S->nseg; i++) {
        i64 lo, hi;
        window(&S->seg[i], &lo, &hi);
        if (lo <= v && v < hi && lo > best_lo) best = i, best_lo = lo;
    }
    return best;
}

static void moved_log(Scan *S, uint32_t epoch, i64 lo, i64 hi) {
    if (S->nmoved == S->capmoved) {
        S->capmoved = S->capmoved ? 2 * S->capmoved : 64;
        S->moved = cs_realloc(S->moved, S->capmoved * sizeof *S->moved);
    }
    S->moved[S->nmoved].epoch = epoch, S->moved[S->nmoved].lo = lo, S->moved[S->nmoved++].hi = hi;
}

// Logs every range that table A maps to a segment and table B maps elsewhere or not at all
static void table_diff(Scan *S, uint32_t ep, const i64 *alo, const i64 *ahi, const int *aseg, uint32_t an,
                       const i64 *blo, const i64 *bhi, const int *bseg, uint32_t bn) {
    for (uint32_t i = 0; i < an; i++) {
        if (aseg[i] < 0) continue;
        i64 covered = alo[i];
        for (uint32_t j = 0; j < bn; j++) {
            i64 lo = blo[j] > alo[i] ? blo[j] : alo[i], hi = bhi[j] < ahi[i] ? bhi[j] : ahi[i];
            if (lo >= hi) continue;
            if (bseg[j] != aseg[i]) moved_log(S, ep, lo, hi);
            if (lo > covered) moved_log(S, ep, covered, lo);
            if (hi > covered) covered = hi;
        }
        if (covered < ahi[i]) moved_log(S, ep, covered, ahi[i]);
    }
}

// Did any address in [lo, hi) change winner after epoch?
static int moved_since(const Scan *S, uint32_t epoch, i64 lo, i64 hi) {
    uint32_t a = 0, b = S->nmoved;
    while (a < b) {
        uint32_t m = (a + b) / 2;
        if (S->moved[m].epoch <= epoch) a = m + 1;
        else b = m;
    }
    for (; a < S->nmoved; a++)
        if (S->moved[a].lo < hi && lo < S->moved[a].hi) return 1;
    return 0;
}

// The window with the greatest lo that holds v, from intervals between window edges, rebuilt when a segment moves
static int segment_of(Scan *S, i64 v) {
    if (S->wc.ver != S->segver + 1) {
        i64 edge[130];
        uint32_t ne = 0;
        for (uint32_t i = 0; i < S->nseg && ne < 128; i++) {
            window(&S->seg[i], &edge[ne], &edge[ne + 1]);
            ne += 2;
        }
        if (S->nseg > 64) {
            S->wc.n = 0, S->wc.ver = 0;
            return segment_of_slow(S, v);
        }
        for (uint32_t i = 1; i < ne; i++) {
            i64 x = edge[i];
            uint32_t j = i;
            while (j > 0 && edge[j - 1] > x) edge[j] = edge[j - 1], j--;
            edge[j] = x;
        }
        i64 olo[64], ohi[64];
        int oseg[64];
        uint32_t on = S->wc.n;
        memcpy(olo, S->wc.lo, sizeof olo), memcpy(ohi, S->wc.hi, sizeof ohi), memcpy(oseg, S->wc.seg, sizeof oseg);
        S->wc.n = 0;
        for (uint32_t i = 0; i + 1 < ne; i++) {
            if (edge[i] == edge[i + 1]) continue;
            S->wc.lo[S->wc.n] = edge[i], S->wc.hi[S->wc.n] = edge[i + 1];
            S->wc.seg[S->wc.n++] = segment_of_slow(S, edge[i]);
        }
        uint32_t ep = S->map_epoch + 1;
        if (on) {
            table_diff(S, ep, olo, ohi, oseg, on, S->wc.lo, S->wc.hi, S->wc.seg, S->wc.n);
            table_diff(S, ep, S->wc.lo, S->wc.hi, S->wc.seg, S->wc.n, olo, ohi, oseg, on);
        }
        if (S->nmoved && S->moved[S->nmoved - 1].epoch == ep) S->map_epoch = ep;
        S->wc.ver = S->segver + 1;
        S->wc.last = 0;
    }
    uint32_t l = S->wc.last;
    if (l < S->wc.n && S->wc.lo[l] <= v && v < S->wc.hi[l]) return S->wc.seg[l];
    uint32_t lo = 0, hi = S->wc.n;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (S->wc.hi[m] <= v) lo = m + 1;
        else hi = m;
    }
    if (lo < S->wc.n && S->wc.lo[lo] <= v) {
        S->wc.last = lo;
        return S->wc.seg[lo];
    }
    return -1;
}

static i64 word(Scan *S, i64 v) {
    int s = segment_of(S, v);
    return s < 0 ? NONE : rom_word(S, v + S->seg[s].delta);
}

static int ov_holds(const Seg *o, uint32_t v) { return o->real <= v && v - o->real < (uint32_t)(o->limit - o->vram); }
static uint32_t ov_key(const Seg *o, uint32_t v) { return (o->vram & ~CS_OV_MASK) | (v & CS_OV_MASK); }
static uint32_t ov_real(uint32_t v) { return CS_OV_IS_KEY(v) ? 0x80000000u | (v & CS_OV_MASK) : v; }

static int in_overlay(const Scan *S, uint32_t v) {
    for (uint32_t i = 0; i < S->novl; i++)
        if (ov_holds(&S->seg[S->ovl[i]], v)) return 1;
    return 0;
}

// v as code in segment ctx sees it, into out[]; returns how many
static uint32_t resolve(const Scan *S, int ctx, uint32_t v, uint32_t *out) {
    if (!S->novl) return out[0] = v, 1;
    if (ctx >= 0 && S->seg[ctx].real && ov_holds(&S->seg[ctx], v)) return out[0] = ov_key(&S->seg[ctx], v), 1;
    if (v >= OV_BASE && v < OV_END) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < S->novl; i++)
        if (ov_holds(&S->seg[S->ovl[i]], v)) out[n++] = ov_key(&S->seg[S->ovl[i]], v);
    if (!n) out[n++] = v;
    return n;
}

static void push_resolved(const Scan *S, int ctx, uint32_t v, Vec *dst) {
    uint32_t k[OV_MAX];
    for (uint32_t i = 0, n = resolve(S, ctx, v, k); i < n; i++) vpush(dst, k[i]);
}

// A data address as code in ctx reads it; NONE in key space
static i64 own_addr(const Scan *S, int ctx, uint32_t v) {
    if (ctx >= 0 && S->seg[ctx].real && ov_holds(&S->seg[ctx], v)) return ov_key(&S->seg[ctx], v);
    return S->novl && v >= OV_BASE && v < OV_END ? NONE : (i64)v;
}

static uint16_t decode_info(uint32_t raw, uint32_t v) {
    RabbitizerInstruction ri;
    RabbitizerInstruction_init(&ri, raw, v);
    RabbitizerInstruction_processUniqueId(&ri);
    uint16_t f = I_PROBED;
    if (RabbitizerInstruction_isValid(&ri)) f |= I_VALID;
    if (RabbitizerInstruction_hasDelaySlot(&ri)) f |= I_DELAY;
    if (RabbitizerInstruction_isJrRa(&ri)) f |= I_JRRA;
    if (RabbitizerInstrDescriptor_doesLink(ri.descriptor)) f |= I_LINK;
    if (RabbitizerInstrDescriptor_modifiesRt(ri.descriptor)) f |= I_MODRT;
    if (RabbitizerInstrDescriptor_modifiesRd(ri.descriptor)) f |= I_MODRD;
    if (RabbitizerInstruction_hasOperandAlias(&ri, RAB_OPERAND_cpu_label)) f |= I_LABEL;
    int c = C_OTHER;
    switch (ri.uniqueId) {
    case RABBITIZER_INSTR_ID_cpu_jr: c = C_JR; break;
    case RABBITIZER_INSTR_ID_cpu_jal: c = C_JAL; break;
    case RABBITIZER_INSTR_ID_cpu_jalr: c = C_JALR; break;
    case RABBITIZER_INSTR_ID_cpu_j: c = C_J; break;
    case RABBITIZER_INSTR_ID_cpu_lui: c = C_LUI; break;
    case RABBITIZER_INSTR_ID_cpu_addiu: c = C_ADDIU; break;
    case RABBITIZER_INSTR_ID_cpu_ori: c = C_ORI; break;
    case RABBITIZER_INSTR_ID_cpu_lw: c = C_LW; break;
    case RABBITIZER_INSTR_ID_cpu_addu: c = C_ADDU; break;
    case RABBITIZER_INSTR_ID_cpu_sll: c = C_SLL; break;
    case RABBITIZER_INSTR_ID_cpu_sltiu: c = C_SLTIU; break;
    default: break;
    }
    return f | c << 8;
}

// The word ins() read at probe time: the cache keeps only which window it came through, whose delta never changes
static uint32_t probed_raw(Scan *S, const Page *p, uint32_t k, uint32_t v) {
    if (p->info[k] & I_NONE) return 0;
    return (uint32_t)rom_word(S, v + S->seg[p->seg[k]].delta);
}

// Decoded once per vram and never invalidated, even when segments later move under it
static Ins ins(Scan *S, uint32_t v) {
    Ins r = {0, I_PROBED | I_NONE};
    if (v & 3) {
        i64 w = word(S, v);
        if (w != NONE) r.raw = (uint32_t)w, r.info = decode_info(r.raw, v);
        return r;
    }
    Page *p = S->last_cpage;
    if (!p || S->last_cpage_key != v >> 12) {
        i64 pi = map_get(&S->cache_pages, v >> 12);
        if (pi == NONE) {
            if (S->npages == S->cappages) {
                S->cappages = S->cappages ? 2 * S->cappages : 64;
                S->pages = cs_realloc(S->pages, S->cappages * sizeof *S->pages);
            }
            S->pages[S->npages] = cs_alloc(sizeof(Page));
            memset(S->pages[S->npages]->info, 0, sizeof S->pages[S->npages]->info);
            pi = S->npages++;
            map_put(&S->cache_pages, v >> 12, (uint32_t)pi);
        }
        p = S->last_cpage = S->pages[pi];
        S->last_cpage_key = v >> 12;
    }
    uint32_t k = (v >> 2) & 1023;
    if (!p->info[k]) {
        int s = segment_of(S, v);
        if (s > 254) cs_die("scan: more segments than the instruction cache can name");
        i64 w = s < 0 ? NONE : rom_word(S, v + S->seg[s].delta);
        p->seg[k] = s < 0 ? 255 : (uint8_t)s;
        p->info[k] = w == NONE ? I_PROBED | I_NONE : decode_info((uint32_t)w, v);
        r.raw = w == NONE ? 0 : (uint32_t)w;
    } else {
        r.raw = probed_raw(S, p, k, v);
    }
    r.info = p->info[k];
    return r;
}

// word(v) for an address already probed: its cached word, unless a different window now maps it
static i64 word_seen(Scan *S, uint32_t v) {
    Page *p = S->last_cpage;
    if (!p || S->last_cpage_key != v >> 12) {
        i64 pi = map_get(&S->cache_pages, v >> 12);
        if (pi == NONE) return word(S, v);
        p = S->last_cpage = S->pages[pi];
        S->last_cpage_key = v >> 12;
    }
    uint32_t k = (v >> 2) & 1023;
    if (!(p->info[k] & I_PROBED) || (p->info[k] & I_NONE) || p->seg[k] == 255) return word(S, v);
    int s = segment_of(S, v);
    return s == p->seg[k] ? (i64)probed_raw(S, p, k, v) : word(S, v);
}

static int ins_ok(Ins i) { return !(i.info & I_NONE) && (i.info & I_VALID); }
static uint32_t index_vram(uint32_t raw, uint32_t v) {
    return ((raw & 0x3FFFFFFu) << 2) | (v == 0 ? 0x80000000u : (v + 4) & 0xF0000000u);
}
static uint32_t branch_vram(Ins i, uint32_t v) {
    return (i.info & I_LABEL) ? index_vram(i.raw, v) : v + 4 + (uint32_t)((int32_t)(int16_t)i.raw * 4);
}
static int32_t simm(uint32_t raw) { return (int16_t)raw; }
static int writes(Ins i, int reg) {
    return ((i.info & I_MODRT) && (int)((i.raw >> 16) & 31) == reg) ||
           ((i.info & I_MODRD) && (int)((i.raw >> 11) & 31) == reg);
}

static int ends_block(i64 w) {
    if (w == NONE) w = 0;
    uint32_t x = (uint32_t)w;
    return x == JR_RA || x == ERET || x >> 26 == 0x02 || (x >> 16) == 0x1000;
}

// Functions
static Func *func_new(uint32_t start) {
    Func *f = cs_alloc(sizeof *f);
    memset(f, 0, sizeof *f);
    f->start = f->end = start;
    f->refs = 1;
    return f;
}

// Drops one holder's reference: the function table, an explore memo, or a caller of explore
static void func_free(Func *f) {
    if (!f || --f->refs) return;
    vfree(&f->calls), vfree(&f->tails), vfree(&f->unres), vfree(&f->absorbed);
    for (uint32_t i = 0; i < f->ntabs; i++) vfree(&f->tabs[i].targets);
    cs_release(f->tabs);
    cs_release(f->seen);
    cs_release(f);
}

static int seen_has(const Func *f, i64 pc) {
    if (pc < f->start || (pc - f->start) & 3) return 0;
    u64 b = (u64)(pc - f->start) >> 2;
    return b < f->seencap && (f->seen[b >> 3] >> (b & 7) & 1);
}

static void seen_add(Func *f, uint32_t pc) {
    if (pc < f->start || (pc - f->start) & 3) cs_die("scan: explore reached an address below its start");
    uint32_t b = (pc - f->start) >> 2;
    if (b >= f->seencap) {
        uint32_t cap = f->seencap ? f->seencap : 256;
        while (cap <= b) cap *= 2;
        f->seen = cs_realloc(f->seen, cap / 8);
        memset(f->seen + f->seencap / 8, 0, (cap - f->seencap) / 8);
        f->seencap = cap;
    }
    if (!(f->seen[b >> 3] >> (b & 7) & 1)) f->nseen++;
    f->seen[b >> 3] |= 1 << (b & 7);
    if (pc > f->maxseen || f->nseen == 1) f->maxseen = pc;
}

// The seen bit at or after b, or seencap (a table: the SH-4 has no ctz, and libgcc's is a call)
static uint32_t next_seen(const Func *f, uint32_t b) {
    static uint8_t low[256];
    if (!low[0]) {
        low[0] = 8;
        for (int v = 1; v < 256; v++) {
            int k = 0;
            while (!(v >> k & 1)) k++;
            low[v] = (uint8_t)k;
        }
    }
    while (b < f->seencap) {
        uint8_t byte = f->seen[b >> 3] >> (b & 7);
        if (byte) return b + low[byte];
        b = (b | 7) + 1;
    }
    return f->seencap;
}

// Every seen pc in address order
#define FOR_SEEN(f, pc)                                                                                     \
    for (uint32_t _b = next_seen((f), 0), pc = (f)->start + 4 * _b; _b < (f)->seencap;                      \
         _b = next_seen((f), _b + 1), pc = (f)->start + 4 * _b)

static Func *get_func(const Scan *S, uint32_t s) {
    i64 i = map_get(&S->funcs, s);
    return i == NONE ? NULL : S->fpool[i];
}

static uint32_t lower_bound(const Vec *v, uint32_t x) {
    uint32_t lo = 0, hi = v->n;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (v->v[m] < x) lo = m + 1;
        else hi = m;
    }
    return lo;
}

// S->sorted keeps every function start in order as the set changes
static void sorted_insert(Scan *S, uint32_t s) {
    uint32_t at = lower_bound(&S->sorted, s);
    vpush(&S->sorted, 0);
    memmove(S->sorted.v + at + 1, S->sorted.v + at, (S->sorted.n - 1 - at) * 4);
    S->sorted.v[at] = s;
}

static void sorted_remove(Scan *S, uint32_t s) {
    uint32_t at = lower_bound(&S->sorted, s);
    if (at < S->sorted.n && S->sorted.v[at] == s) {
        memmove(S->sorted.v + at, S->sorted.v + at + 1, (S->sorted.n - 1 - at) * 4);
        S->sorted.n--;
    }
}

static void put_func(Scan *S, uint32_t s, Func *f) {
    i64 i = map_get(&S->funcs, s);
    if (i != NONE) {
        func_free(S->fpool[i]);
        S->fpool[i] = f;
        return;
    }
    uint32_t k;
    if (S->ffree.n) k = S->ffree.v[--S->ffree.n];
    else {
        if (S->nfpool == S->capfpool) {
            S->capfpool = S->capfpool ? 2 * S->capfpool : 1024;
            S->fpool = cs_realloc(S->fpool, S->capfpool * sizeof *S->fpool);
        }
        k = S->nfpool++;
    }
    S->fpool[k] = f;
    map_put(&S->funcs, s, k);
    sorted_insert(S, s);
}

// Unlinks s; frees its Func unless keep
static void unlink_func(Scan *S, uint32_t s, int keep) {
    i64 i = map_get(&S->funcs, s);
    if (i == NONE) return;
    if (!keep) func_free(S->fpool[i]);
    S->fpool[i] = NULL;
    vpush(&S->ffree, (uint32_t)i);
    map_del(&S->funcs, s);
    sorted_remove(S, s);
}

static void del_func(Scan *S, uint32_t s) { unlink_func(S, s, 0); }

static const Vec *sorted_view(Scan *S) { return &S->sorted; }

static Vec sorted_starts(Scan *S) {
    const Vec *s = sorted_view(S);
    Vec v = {0};
    v.n = v.cap = s->n;
    v.v = cs_alloc((s->n + 1) * 4);
    memcpy(v.v, s->v, s->n * 4);
    return v;
}

// Explore and its register tracking
static i64 def_(Scan *S, const Func *f, i64 pc, int reg, i64 floor, Ins *out) {
    for (i64 p = pc - 4; p >= floor && seen_has(f, p); p -= 4) {
        Ins i = ins(S, (uint32_t)p);
        if (writes(i, reg)) {
            *out = i;
            return p;
        }
    }
    return NONE;
}

static i64 reg_const(Scan *S, const Func *f, uint32_t pc, int reg) {
    Ins d, d2;
    i64 p = def_(S, f, pc, reg, (i64)pc - 0x40, &d);
    if (p == NONE || (I_CLASS(d.info) != C_ADDIU && I_CLASS(d.info) != C_ORI)) return NONE;
    i64 p2 = def_(S, f, p, (d.raw >> 21) & 31, p - 0x40, &d2);
    if (p2 == NONE || I_CLASS(d2.info) != C_LUI) return NONE;
    int64_t lo = I_CLASS(d.info) == C_ADDIU ? simm(d.raw) : (d.raw & 0xFFFF);
    return (uint32_t)(((d2.raw & 0xFFFF) << 16) + lo);
}

static i64 table_bound(Scan *S, const Func *f, int reg, uint32_t pc, i64 floor) {
    Ins d;
    i64 p = def_(S, f, pc, reg, f->start, &d);  // IDO can scale the index well before the dispatch
    if (p != NONE && I_CLASS(d.info) == C_SLL && ((d.raw >> 6) & 31) == 2) reg = (d.raw >> 16) & 31;
    for (i64 q = (i64)pc - 4; q >= floor && seen_has(f, q); q -= 4) {
        Ins i = ins(S, (uint32_t)q);
        if (I_CLASS(i.info) == C_SLTIU && (int)((i.raw >> 21) & 31) == reg) return i.raw & 0xFFFF;
        if (q != p && writes(i, reg)) return NONE;
    }
    return NONE;
}

static Vec *jump_table(Scan *S, Func *f, uint32_t jr) {
    i64 floor = (i64)jr - 0x80;
    Ins jri = ins(S, jr), lw, add, d, d2;
    i64 lw_pc = def_(S, f, jr, (jri.raw >> 21) & 31, floor, &lw);
    if (lw_pc == NONE || I_CLASS(lw.info) != C_LW) return NULL;
    int base = (lw.raw >> 21) & 31;
    i64 lo = simm(lw.raw);
    i64 add_pc = def_(S, f, lw_pc, base, floor, &add);
    if (add_pc == NONE) return NULL;
    if (I_CLASS(add.info) == C_ADDIU && lo == 0) {
        lo = simm(add.raw);
        add_pc = def_(S, f, add_pc, (add.raw >> 21) & 31, floor, &add);
    }
    if (add_pc == NONE || I_CLASS(add.info) != C_ADDU) return NULL;
    uint32_t araw = add.raw;
    int have = 0, ireg = 0;
    i64 hi = 0;
    int pairs[2][2] = {{(araw >> 21) & 31, (araw >> 16) & 31}, {(araw >> 16) & 31, (araw >> 21) & 31}};
    for (int k = 0; k < 2 && !have; k++) {
        int r = pairs[k][0], other = pairs[k][1];
        i64 p = def_(S, f, add_pc, r, floor, &d);
        if (p != NONE && I_CLASS(d.info) == C_LUI) {
            hi = (i64)(d.raw & 0xFFFF) << 16, ireg = other, have = 1;
            break;
        }
        if (p != NONE && I_CLASS(d.info) == C_ADDIU && lo == 0) {
            i64 p2 = def_(S, f, p, (d.raw >> 21) & 31, floor, &d2);
            if (p2 != NONE && I_CLASS(d2.info) == C_LUI) {
                hi = ((i64)(d2.raw & 0xFFFF) << 16) + simm(d.raw), ireg = other, have = 1;
                break;
            }
        }
    }
    if (!have) return NULL;
    int ctx = segment_of(S, f->start);
    i64 tk = own_addr(S, ctx, (uint32_t)(hi + lo));
    if (tk == NONE) return NULL;
    uint32_t table = (uint32_t)tk;
    i64 count = table_bound(S, f, ireg, (uint32_t)add_pc, floor);
    int bounded = count != NONE && count != 0;
    Vec targets = {0};
    S->dep_words = 1;
    if (!bounded) S->dep_unbounded = 1;
    for (uint32_t i = 0; i < (bounded ? (uint32_t)count : 1024u); i++) {
        i64 t = word(S, (i64)table + 4 * i);
        if (t != NONE) t = own_addr(S, ctx, (uint32_t)t);
        if (t == NONE || (t & 3) || !(f->start <= t && t < (i64)f->start + 0x10000)) break;
        dep(S, (uint32_t)t);
        if (bits_has(&S->strong, (uint32_t)t) && t != f->start) break;
        if (!bounded && i && bits_has(&S->table_entries, (uint32_t)t)) break;
        vpush(&targets, (uint32_t)t);
    }
    if (!targets.n) return NULL;
    if (f->ntabs == f->captabs) {
        f->captabs = f->captabs ? 2 * f->captabs : 4;
        f->tabs = cs_realloc(f->tabs, f->captabs * sizeof *f->tabs);
    }
    Tab *tb = &f->tabs[f->ntabs++];
    tb->jr = jr, tb->table = table, tb->targets = targets;
    for (uint32_t i = 0; i < targets.n; i++) bits_add(&S->table_entries, table + 4 * i);
    return &tb->targets;
}

// The jr's met so far: a constant target is a tail, a jump table's entries are more code, the rest stay unresolved
static void resolve_pending(Scan *S, Func *f, int ctx, Vec *pending, Vec *todo) {
    for (uint32_t k = 0; k < pending->n; k++) {
        uint32_t jr = pending->v[k];
        i64 c = reg_const(S, f, jr, (ins(S, jr).raw >> 21) & 31);
        if (c != NONE) {
            push_resolved(S, ctx, (uint32_t)c, &f->tails);
            continue;
        }
        Vec *t = jump_table(S, f, jr);
        if (!t) vpush(&f->unres, jr);
        else
            for (uint32_t i = 0; i < t->n; i++) vpush(todo, t->v[i]);
    }
    pending->n = 0;
}

// What the delay-slot instruction i at pc adds to f and the work lists; 1 if its run goes on past the slot
__attribute__((always_inline)) static inline int follow_branch(Scan *S, Func *f, int ctx, uint32_t start, Ins i, uint32_t pc,
                                                               Vec *todo, Vec *pending) {
    if (i.info & I_JRRA) return 0;
    int c = I_CLASS(i.info);
    if (c == C_JR) {
        vpush(pending, pc);
        return 0;
    }
    if (c == C_JAL) {
        push_resolved(S, ctx, index_vram(i.raw, pc), &f->calls);
        return 1;
    }
    if (c == C_JALR) return 1;
    if (c == C_J) {
        uint32_t ts[OV_MAX], nt = resolve(S, ctx, index_vram(i.raw, pc), ts);
        if (nt != 1) {
            for (uint32_t k = 0; k < nt; k++) vpush(&f->tails, ts[k]);
            return 0;
        }
        uint32_t t = ts[0];
        if (t >= start) dep(S, t);
        if (t < start || (t != start && bits_has(&S->strong, t))) vpush(&f->tails, t);
        else vpush(todo, t);
        return 0;
    }
    uint32_t t = branch_vram(i, pc);
    if (!(i.info & I_LINK)) dep(S, t);
    if (i.info & I_LINK) {
        if ((i.raw >> 16) != 0x0410) vpush(&f->calls, t);  // bltzal $zero only links: its target is data
    } else if (t < start || (t != start && bits_has(&S->strong, t))) vpush(&f->tails, t);
    else if (t >= start) vpush(todo, t);
    return (i.raw >> 16) != 0x1000 && (i.raw >> 16) != 0x0401;
}

// One straight run from pc until it ends or joins code already seen; 0 if it reaches a word that isn't code
__attribute__((always_inline)) static inline int walk_run(Scan *S, Func *f, int ctx, uint32_t start, uint32_t pc, Vec *todo,
                                                          Vec *pending) {
    while (!seen_has(f, pc)) {
        dep(S, pc);
        if (pc != start && bits_has(&S->starts, pc)) {
            if (bits_has(&S->strong, pc)) {
                vpush(&f->tails, pc);
                return 1;
            }
            vpush(&f->absorbed, pc);
        }
        Ins i = ins(S, pc);
        if (!ins_ok(i)) {
            map_put(&S->bad, start, pc);
            return 0;
        }
        seen_add(f, pc);
        if (!(i.info & I_DELAY)) {
            if (i.raw == ERET) return 1;
            pc += 4;
            continue;
        }
        Ins sl = ins(S, pc + 4);
        if (!ins_ok(sl) || (sl.info & I_DELAY)) {
            map_put(&S->bad, start, pc + 4);
            return 0;
        }
        seen_add(f, pc + 4);
        if (!follow_branch(S, f, ctx, start, i, pc, todo, pending)) return 1;
        pc += 8;
    }
    return 1;
}

static Func *explore_raw(Scan *S, uint32_t start) {
    Func *f = func_new(start);
    int ctx = segment_of(S, start);
    Vec todo = {0}, pending = {0};
    vpush(&todo, start);
    while (todo.n || pending.n) {
        if (!todo.n) {
            resolve_pending(S, f, ctx, &pending, &todo);
        } else if (!walk_run(S, f, ctx, start, todo.v[--todo.n], &todo, &pending)) {
            vfree(&todo), vfree(&pending);
            func_free(f);
            return NULL;
        }
    }
    vfree(&todo), vfree(&pending);
    i64 de = map_get(&S->dead_end, start);
    uint32_t end = f->maxseen + 4;
    if (de != NONE && (uint32_t)de > end) end = (uint32_t)de;
    f->end = end;
    vsortu(&f->calls), vsortu(&f->tails);
    return f;
}

// f is about to change (its end): the memo stops sharing it, and its entry is stale anyway once dead_end moves
static void own(Scan *S, Func *f) {
    f->memo_gen = 0;
    if (f->refs < 2) return;
    i64 mi = map_get(&S->memo_of, f->start);
    if (mi != NONE && S->memo[mi].f == f) {
        S->memo[mi].f = NULL, S->memo[mi].valid = 0;
        f->refs--;
    }
}

// explore_raw's answer depends only on what it records in deps, so an unchanged input replays the last answer.
// *same is set when old is that very answer. The answer is shared with the memo, not copied.
static Func *explore_same(Scan *S, uint32_t start, const Func *old, int *same) {
    *same = 0;
    i64 mi = map_get(&S->memo_of, start);
    if (mi != NONE) {
        Memo *m = &S->memo[mi];
        int ok = m->valid && !m->unbounded && (!m->words || m->segver == S->segver) && m->novl == S->novl;
        for (uint32_t i = 0; i < m->deps.n && ok; i++) ok = blk_epoch(S, m->deps.v[i]) <= m->stamp;
        if (ok) {
            if (m->bad) {
                map_put(&S->bad, start, m->badpc);
                return NULL;
            }
            for (uint32_t t = 0; t < m->f->ntabs; t++)
                for (uint32_t j = 0; j < m->f->tabs[t].targets.n; j++)
                    bits_add(&S->table_entries, m->f->tabs[t].table + 4 * j);
            if (old && old == m->f && old->memo_gen == m->gen) {
                *same = 1;
                return (Func *)old;
            }
            m->f->refs++;
            return m->f;
        }
    } else {
        if (S->nmemo == S->capmemo) {
            S->capmemo = S->capmemo ? 2 * S->capmemo : 1024;
            S->memo = cs_realloc(S->memo, S->capmemo * sizeof *S->memo);
        }
        memset(&S->memo[S->nmemo], 0, sizeof *S->memo);
        mi = S->nmemo++;
        map_put(&S->memo_of, start, (uint32_t)mi);
    }
    Memo *m = &S->memo[mi];
    func_free(m->f);
    m->f = NULL;
    m->deps.n = 0;
    S->deps = &m->deps;
    S->dep_words = S->dep_unbounded = 0;
    m->stamp = S->epoch;
    Func *f = explore_raw(S, start);
    S->deps = NULL;
    vsortu(&m->deps);
    m->segver = S->segver, m->words = S->dep_words, m->unbounded = S->dep_unbounded, m->novl = S->novl;
    m->gen = ++S->memo_gen;
    m->bad = !f;
    m->valid = 1;
    if (f) {
        f->memo_gen = m->gen;
        f->refs++;
        m->f = f;
    } else {
        m->badpc = (uint32_t)map_get(&S->bad, start);
    }
    return f;
}

static Func *explore(Scan *S, uint32_t start) {
    int same;
    return explore_same(S, start, NULL, &same);
}

// Closure and refinement
static void strengthen(Scan *S, const Func *f) {
    for (uint32_t i = 0; i < f->calls.n; i++) mark_add(S, &S->strong, f->calls.v[i]);
    if (bits_has(&S->strong, f->start))
        for (uint32_t i = 0; i < f->tails.n; i++) mark_add(S, &S->strong, f->tails.v[i]);
}

static uint32_t absorb(Scan *S, const Func *f) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < f->absorbed.n; i++) {
        uint32_t w = f->absorbed.v[i];
        if (!bits_has(&S->strong, w)) {
            del_func(S, w);
            mark_del(S, &S->starts, w);
            n++;
        }
    }
    return n;
}

// sorted(calls | tails), appended to q
static void push_union(Vec *q, const Func *f) {
    uint32_t a = 0, b = 0;
    while (a < f->calls.n || b < f->tails.n) {
        uint32_t x;
        if (b >= f->tails.n || (a < f->calls.n && f->calls.v[a] < f->tails.v[b])) x = f->calls.v[a++];
        else if (a >= f->calls.n || f->tails.v[b] < f->calls.v[a]) x = f->tails.v[b++];
        else x = f->calls.v[a++], b++;
        vpush(q, x);
    }
}

typedef struct { uint32_t start, end; } Span;

static Span *extent_index(Scan *S, uint32_t *n) {
    const Vec *st = sorted_view(S);
    Span *sp = cs_alloc((st->n + 1) * sizeof *sp);
    for (uint32_t i = 0; i < st->n; i++) sp[i].start = st->v[i], sp[i].end = get_func(S, st->v[i])->end;
    *n = st->n;
    return sp;
}

static int refine(Scan *S) {
    int changed = 0;
    for (;;) {
        int rc = 0;
        bits_clear(&S->table_entries);
        Vec keys = sorted_starts(S);
        for (uint32_t k = 0; k < keys.n; k++) {
            uint32_t s = keys.v[k];
            Func *old = get_func(S, s);
            if (!old) continue;
            int same;
            Func *f = explore_same(S, s, old, &same);
            if (!f) {
                del_func(S, s);
                mark_del(S, &S->starts, s);
                rc = 1;
                continue;
            }
            if (!same) {
                if (f->end != old->end || !veq(&f->calls, &old->calls) || !veq(&f->tails, &old->tails)) rc = 1;
                put_func(S, s, f);
            }
            if (absorb(S, f)) rc = 1;
            strengthen(S, f);
        }
        vfree(&keys);
        uint32_t n;
        Span *sp = extent_index(S, &n);
        for (uint32_t i = 1; i < n; i++) {
            uint32_t s = sp[i].start;
            if (!bits_has(&S->strong, s) && sp[i - 1].end > s && map_has(&S->funcs, s)) {
                del_func(S, s);
                mark_del(S, &S->starts, s);
                rc = 1;
            }
        }
        cs_release(sp);
        if (!rc) return changed;
        changed = 1;
    }
}

static void run_closure(Scan *S, Vec *queue, uint32_t origin, int strong) {
    if (strong)
        for (uint32_t i = 0; i < queue->n; i++) mark_add(S, &S->strong, queue->v[i]);
    for (;;) {
        while (queue->n) {
            uint32_t s = queue->v[--queue->n];
            if (map_has(&S->funcs, s) || map_has(&S->bad, s) || in_overlay(S, s)) continue;
            if (segment_of(S, s) < 0 || (s & 3)) {
                if (!map_has(&S->unplaceable, s)) map_add(&S->unplaced, s);
                continue;
            }
            mark_add(S, &S->starts, s);
            Func *f = explore(S, s);
            if (!f) {
                mark_del(S, &S->starts, s);
                continue;
            }
            put_func(S, s, f);
            if (!map_has(&S->origin, s)) map_put(&S->origin, s, origin);
            absorb(S, f);
            push_union(queue, f);
            strengthen(S, f);
        }
        if (!refine(S)) break;
        Vec keys = sorted_starts(S);
        for (uint32_t k = 0; k < keys.n; k++) push_union(queue, get_func(S, keys.v[k]));
        vfree(&keys);
    }
}

static i64 arg_const(Scan *S, const Func *f, uint32_t pc, int reg) {
    i64 c = reg_const(S, f, pc, reg);
    if (c != NONE) return c;
    Ins d;
    i64 p = def_(S, f, pc, reg, (i64)pc - 0x40, &d);
    return p != NONE && I_CLASS(d.info) == C_LUI ? (i64)((d.raw & 0xFFFF) << 16) : NONE;
}

static Seg *add_seg(Scan *S, uint32_t vram, i64 rom, uint32_t size, uint32_t how) {
    if (S->nseg == S->capseg) {
        S->capseg = S->capseg ? 2 * S->capseg : 8;
        S->seg = cs_realloc(S->seg, S->capseg * sizeof *S->seg);
    }
    Seg *s = &S->seg[S->nseg++];
    S->segver++;
    s->vram = s->lo = vram;
    s->hi = vram + 4;
    s->delta = rom - vram;
    s->limit = size ? (u64)vram + size : 0;
    s->how = how, s->how_n = 0, s->real = 0;
    return s;
}

// Whether f invalidates the I-cache, and its jal pcs, from word() over its seen pcs; kept with f's memo entry while
// the window table gives every address the same segment
static Vec ic_scratch, pv_scratch;  // cs_free returns them: a console boot frees the scan's memory for good

static const Vec *ic_facts(Scan *S, const Func *f, int *inval) {
    if (S->wc.ver != S->segver + 1) segment_of(S, 0);
    i64 mi = f->memo_gen ? map_get(&S->memo_of, f->start) : NONE;
    Memo *m = mi != NONE && S->memo[mi].gen == f->memo_gen ? &S->memo[mi] : NULL;
    if (m && m->ic_gen == m->gen &&
        (m->ic_epoch == S->map_epoch || !moved_since(S, m->ic_epoch, f->start, (i64)f->maxseen + 4))) {
        m->ic_epoch = S->map_epoch;
        *inval = m->ic_inval;
        return &m->ic_calls;
    }
    Vec *calls = m ? &m->ic_calls : &ic_scratch;
    calls->n = 0;
    *inval = 0;
    FOR_SEEN(f, p) {
        i64 w = word_seen(S, p);
        if (w == NONE) continue;
        if (((uint32_t)w >> 26) == CACHE_OP && (((uint32_t)w >> 16) & 31) == HIT_INV_I) *inval = 1;
        if (((uint32_t)w >> 26) == JAL_OP) vpush(calls, p);
    }
    if (m) m->ic_gen = m->gen, m->ic_epoch = S->map_epoch, m->ic_inval = (uint8_t)*inval;
    return calls;
}

static uint32_t icache_segments(Scan *S) {
    Vec keys = sorted_starts(S);
    Map inval = {0};
    for (uint32_t k = 0; k < keys.n; k++) {
        int iv;
        ic_facts(S, get_func(S, keys.v[k]), &iv);
        if (iv) map_add(&inval, keys.v[k]);
    }
    uint32_t found = 0;
    Vec calls = {0};
    for (uint32_t k = 0; k < keys.n; k++) {
        Func *f = get_func(S, keys.v[k]);
        int iv;
        const Vec *fc = ic_facts(S, f, &iv);
        int ctx = segment_of(S, f->start);
        calls.n = 0;
        for (uint32_t i = 0; i < fc->n; i++) vpush(&calls, fc->v[i]);
        for (uint32_t c = 0; c < calls.n; c++) {
            uint32_t p = calls.v[c];
            uint32_t site = ((p + 4) & 0xF0000000u) | (((uint32_t)word(S, p) & 0x3FFFFFFu) << 2);
            uint32_t ks[OV_MAX], nk = resolve(S, ctx, site, ks), hit = 0;
            for (uint32_t i = 0; i < nk && !hit; i++) hit = map_has(&inval, ks[i]);
            if (!hit) continue;
            i64 vram = arg_const(S, f, p + 8, 4);
            if (vram == NONE || ((uint32_t)vram >> 29) != 4) continue;
            int placed = 0;
            for (uint32_t i = 0; i < S->nseg && !placed; i++)
                placed = S->seg[i].vram == vram || (S->seg[i].lo <= vram && vram < S->seg[i].hi);
            if (placed) continue;
            if (S->novl && ((uint32_t)vram >= OV_BASE || in_overlay(S, (uint32_t)vram))) continue;
            i64 size = arg_const(S, f, p + 8, 5);
            i64 rom = NONE;
            int nroms = 0;
            for (uint32_t q = 0; q < calls.n; q++) {
                if (calls.v[q] == p || arg_const(S, f, calls.v[q] + 8, 4) != vram) continue;
                i64 r = arg_const(S, f, calls.v[q] + 8, 5);
                if (r == NONE || r == size || r < BOOT_ROM || r >= S->size) continue;
                if (nroms == 0 || r != rom) rom = r, nroms++;
                if (nroms > 1) break;
            }
            if (nroms != 1) continue;
            Seg *same = NULL;
            for (uint32_t i = 0; i < S->nseg && !same; i++)
                if (S->seg[i].delta == rom - vram) same = &S->seg[i];
            if (same) {
                if (vram < same->lo) same->lo = (uint32_t)vram, S->segver++;
                continue;
            }
            add_seg(S, (uint32_t)vram, rom, 0, CS_HOW_ICACHE);
            found++;
        }
    }
    vfree(&calls), vfree(&keys), map_free(&inval);
    if (found) {
        Map keep = {0};
        for (uint32_t i = 0; i < S->unplaced.cap; i++)
            if (S->unplaced.st[i] == 1 && segment_of(S, S->unplaced.key[i]) < 0) map_add(&keep, S->unplaced.key[i]);
        map_free(&S->unplaced);
        S->unplaced = keep;
    }
    return found;
}

static uint32_t rom_off(uint32_t w) { return (w >> 28) == 0x1 || (w >> 28) == 0xB ? w & 0x0FFFFFFFu : w; }

// reg at pc from constants: lui, addiu/ori, addu/subu/or (moves too), and a reload of the stack slot it was stored in
static i64 val_const(Scan *S, const Func *f, i64 pc, int reg, int depth) {
    if (reg == 0) return 0;
    Ins d;
    i64 p = depth < 8 ? def_(S, f, pc, reg, pc - 0x40, &d) : NONE;
    if (p == NONE) return NONE;
    uint32_t w = d.raw, op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, fn = w & 63;
    if (op == 0x0F) return (uint32_t)((w & 0xFFFF) << 16);
    if (op == 0x09 || op == 0x0D) {
        i64 b = val_const(S, f, p, (int)rs, depth + 1);
        if (b == NONE) return NONE;
        return op == 0x09 ? (uint32_t)((uint32_t)b + (uint32_t)simm(w)) : ((uint32_t)b | (w & 0xFFFF));
    }
    if (op == 0 && (fn == 0x21 || fn == 0x23 || fn == 0x25)) {
        i64 a = val_const(S, f, p, (int)rs, depth + 1), b = a == NONE ? NONE : val_const(S, f, p, (int)rt, depth + 1);
        if (b == NONE) return NONE;
        return fn == 0x21 ? (uint32_t)(a + b) : fn == 0x23 ? (uint32_t)(a - b) : (uint32_t)(a | b);
    }
    if (op != 0x23 || rs != 29) return NONE;
    for (i64 q = p - 4; q >= p - 0x40 && seen_has(f, q); q -= 4) {
        Ins s = ins(S, (uint32_t)q);
        if ((s.raw >> 26) == 0x2B && ((s.raw >> 21) & 31) == 29 && (s.raw & 0xFFFF) == (w & 0xFFFF))
            return val_const(S, f, q, (int)((s.raw >> 16) & 31), depth + 1);
        if (writes(s, 29)) return NONE;
    }
    return NONE;
}

// A loader call's (romStart, romEnd, vram) whose range holds an unplaced call target on a function start
static void call_cand(Scan *S, const Vec *targets, uint32_t r0, uint32_t r1, uint32_t v, Vec *cv, Vec *cr, Vec *cn) {
    uint32_t n = r1 - r0;
    if (!(BOOT_ROM <= r0 && r0 < r1 && r1 <= S->size && !(r0 & 3) && n <= OV_LEN_MAX)) return;
    if ((v & 3) || v < OV_RDRAM_LO || (u64)v + n > OV_RDRAM_HI) return;
    int hit = 0;
    for (uint32_t i = 0; i < targets->n && !hit; i++) {
        uint32_t t = targets->v[i];
        hit = v <= t && t - v < n && (t == v || map_has(&S->rom_starts_set, t - v + r0));
    }
    if (hit) vpush(cv, v), vpush(cr, r0), vpush(cn, n);
}

// Code a loader DMAs in from constant f(romStart, romEnd, vram) or f(vram, romStart, size)
static uint32_t call_segments(Scan *S) {
    Vec targets = {0}, keys = sorted_starts(S), calls = {0}, cv = {0}, cr = {0}, cn = {0};
    for (uint32_t i = 0; i < S->unplaced.cap; i++)
        if (S->unplaced.st[i] == 1) vpush(&targets, S->unplaced.key[i]);
    for (uint32_t k = 0; k < keys.n; k++) {
        Func *f = get_func(S, keys.v[k]);
        int iv;
        const Vec *fc = ic_facts(S, f, &iv);
        calls.n = 0;
        for (uint32_t i = 0; i < fc->n; i++) vpush(&calls, fc->v[i]);
        for (uint32_t c = 0; c < calls.n; c++) {
            uint32_t p = calls.v[c];
            i64 a0 = val_const(S, f, p + 8, 4, 0), a1 = val_const(S, f, p + 8, 5, 0), a2 = val_const(S, f, p + 8, 6, 0);
            if (a0 == NONE || a1 == NONE || a2 == NONE) continue;
            call_cand(S, &targets, rom_off((uint32_t)a0), rom_off((uint32_t)a1), (uint32_t)a2, &cv, &cr, &cn);
            call_cand(S, &targets, rom_off((uint32_t)a1), rom_off((uint32_t)a1) + (uint32_t)a2, (uint32_t)a0, &cv, &cr,
                      &cn);
        }
    }
    uint32_t found = 0;
    for (i64 last = -1;;) {  // each vram once, ascending
        i64 v = NONE;
        for (uint32_t i = 0; i < cv.n; i++)
            if ((i64)cv.v[i] > last && (v == NONE || cv.v[i] < v)) v = cv.v[i];
        if (v == NONE) break;
        last = v;
        uint32_t r0 = 0, n = 0, nr = 0;
        for (uint32_t i = 0; i < cv.n; i++)
            if (cv.v[i] == v && (!nr || cr.v[i] != r0 || cn.v[i] != n)) r0 = cr.v[i], n = cn.v[i], nr++;
        if (nr != 1 || (S->novl && in_overlay(S, (uint32_t)v))) continue;
        int clash = 0;
        for (uint32_t i = 0; i < S->nseg && !clash; i++) {
            if (S->seg[i].real) continue;
            i64 lo, hi;
            window(&S->seg[i], &lo, &hi);
            clash = lo < v + n && v < hi;
        }
        if (clash) continue;
        add_seg(S, (uint32_t)v, r0, n, CS_HOW_ARGS);
        found++;
    }
    vfree(&targets), vfree(&keys), vfree(&calls), vfree(&cv), vfree(&cr), vfree(&cn);
    if (found) {
        Map keep = {0};
        for (uint32_t i = 0; i < S->unplaced.cap; i++)
            if (S->unplaced.st[i] == 1 && segment_of(S, S->unplaced.key[i]) < 0) map_add(&keep, S->unplaced.key[i]);
        map_free(&S->unplaced);
        S->unplaced = keep;
    }
    return found;
}

// Overlays from (romStart, romEnd, vram) words in the boot segment
enum { TE_BAD, TE_KNOWN, TE_NEW };
#define TABLE_STRIDE_MAX 64u

// A (romStart, romEnd, vram) candidate at ROM p: malformed, an overlay already placed, or a new one
static int table_word(Scan *S, uint32_t p, uint32_t *r0p, uint32_t *np, uint32_t *vp) {
    uint32_t r0 = rom_off((uint32_t)rom_word(S, p)), r1 = rom_off((uint32_t)rom_word(S, p + 4));
    uint32_t v = (uint32_t)rom_word(S, p + 8), n = r1 - r0;
    if (!(BOOT_ROM <= r0 && r0 < r1 && r1 <= S->size && !(r0 & 3) && n <= OV_LEN_MAX)) return TE_BAD;
    if ((v & 3) || v < OV_RDRAM_LO || (u64)v + n > OV_RDRAM_HI) return TE_BAD;
    for (uint32_t i = 0; i < S->novl; i++) {
        const Seg *o = &S->seg[S->ovl[i]];
        if (o->delta + o->vram == r0 && o->real == v) return TE_KNOWN;
    }
    for (uint32_t i = 0; i < S->nseg; i++) {
        if (S->seg[i].real) continue;
        i64 lo, hi;
        window(&S->seg[i], &lo, &hi);
        if (lo < (i64)v + n && (i64)v < hi) return TE_BAD;
    }
    *r0p = r0, *np = n, *vp = v;
    return TE_NEW;
}

// An unplaced call target lands on a function start in it
static int table_hit(Scan *S, const Vec *targets, uint32_t r0, uint32_t n, uint32_t v) {
    for (uint32_t i = 0; i < targets->n; i++) {
        uint32_t t = targets->v[i];
        if (v <= t && t - v < n && (t == v || map_has(&S->rom_starts_set, t - v + r0))) return 1;
    }
    return 0;
}

static void add_overlay(Scan *S, uint32_t r0, uint32_t n, uint32_t v) {
    if (S->novl == OV_MAX) cs_die("scan: more overlays than the key space holds");
    uint32_t key = OV_BASE + S->novl * OV_SPAN + (v & CS_OV_MASK);
    Seg *s = add_seg(S, key, r0, n, CS_HOW_TABLE);
    s->real = v;
    S->ovl[S->novl++] = S->nseg - 1;
}

// Every entry of one table: two confirmed entries give the stride, and the walk takes each well-formed neighbor
static uint32_t table_walk(Scan *S, const Vec *conf, uint32_t lo, uint32_t end) {
    uint32_t stride = 0, found = 0, r0, n, v;
    for (uint32_t i = 1; i < conf->n; i++) {
        uint32_t g = conf->v[i] - conf->v[i - 1];
        if (g >= 12 && g <= TABLE_STRIDE_MAX && (!stride || g < stride)) stride = g;
    }
    for (uint32_t i = 0; stride && i < conf->n; i++)
        for (int dir = -1; dir <= 1; dir += 2)
            for (i64 q = (i64)conf->v[i] + dir * (i64)stride; q >= lo && q + 8 < end; q += dir * (i64)stride) {
                int e = table_word(S, (uint32_t)q, &r0, &n, &v);
                if (e == TE_BAD) break;
                if (e == TE_NEW) add_overlay(S, r0, n, v), found++;
            }
    return found;
}

static uint32_t table_segments(Scan *S) {
    Vec targets = {0};
    for (uint32_t i = 0; i < S->unplaced.cap; i++)
        if (S->unplaced.st[i] == 1) vpush(&targets, S->unplaced.key[i]);
    uint32_t nspan = S->novl + 1, found = 0, r0, n, v;  // the boot segment, then the overlays placed before this call
    for (uint32_t k = 0; k < nspan; k++) {
        const Seg *o = k ? &S->seg[S->ovl[k - 1]] : &S->seg[0];
        uint32_t lo = (uint32_t)(o->vram + o->delta), end = (uint32_t)(o->limit + o->delta);  // add_seg can move o
        Vec conf = {0};
        for (uint32_t p = lo; p + 8 < end; p += 4) {
            int e = table_word(S, p, &r0, &n, &v);
            if (e == TE_NEW && table_hit(S, &targets, r0, n, v)) add_overlay(S, r0, n, v), found++, e = TE_KNOWN;
            if (e == TE_KNOWN) vpush(&conf, p);
        }
        found += table_walk(S, &conf, lo, end);
        vfree(&conf);
    }
    vfree(&targets);
    if (found) {
        Map keep = {0};
        for (uint32_t i = 0; i < S->unplaced.cap; i++)
            if (S->unplaced.st[i] == 1 && !in_overlay(S, S->unplaced.key[i])) map_add(&keep, S->unplaced.key[i]);
        map_free(&S->unplaced);
        S->unplaced = keep;
    }
    return found;
}

static void rom_starts_done(Scan *S) {
    vsortu(&S->rom_starts);
    for (uint32_t i = 0; i < S->rom_starts.n; i++) map_add(&S->rom_starts_set, S->rom_starts.v[i]);
    vsortu(&S->ptr_words);
}

static uint32_t place_segments(Scan *S) {
    Vec rem = {0};
    for (uint32_t i = 0; i < S->unplaced.cap; i++)
        if (S->unplaced.st[i] == 1 && !map_has(&S->unplaceable, S->unplaced.key[i])) vpush(&rem, S->unplaced.key[i]);
    vsortu(&rem);
    map_clear(&S->unplaced);
    uint32_t found = 0;
    i64 *own = NULL;
    while (rem.n) {
        uint32_t t0 = rem.v[0];
        own = cs_realloc(own, (2 * S->nseg + 2) * sizeof *own);
        for (uint32_t i = 0; i < S->nseg; i++)
            own[2 * i] = S->seg[i].lo + S->seg[i].delta, own[2 * i + 1] = S->seg[i].hi + S->seg[i].delta;
        i64 best_d = 0;
        uint32_t score = 0, second = 0;
        int any = 0;
        for (uint32_t k = 0; k < S->rom_starts.n; k++) {
            i64 p = S->rom_starts.v[k];
            int owned = 0;
            for (uint32_t i = 0; i < S->nseg && !owned; i++) owned = own[2 * i] <= p && p < own[2 * i + 1];
            if (owned) continue;
            i64 d = p - t0;
            uint32_t v = 0;
            for (uint32_t i = 0; i < rem.n; i++) {
                i64 x = rem.v[i] + d;
                if (x >= 0 && x <= 0xFFFFFFFFLL && map_has(&S->rom_starts_set, (uint32_t)x)) v++;
            }
            if (!any || v > score) second = any ? score : 0, score = v, best_d = d, any = 1;
            else if (v > second) second = v;
        }
        if (!any) score = second = 0;
        if (score < 2 || score == second) {
            map_add(&S->unplaceable, t0);
            memmove(rem.v, rem.v + 1, (rem.n - 1) * 4);
            rem.n--;
            continue;
        }
        uint32_t hmin = 0xFFFFFFFFu, hmax = 0;
        Vec left = {0};
        for (uint32_t i = 0; i < rem.n; i++) {
            i64 x = rem.v[i] + best_d;
            if (x >= 0 && x <= 0xFFFFFFFFLL && map_has(&S->rom_starts_set, (uint32_t)x)) {
                if (rem.v[i] < hmin) hmin = rem.v[i];
                if (rem.v[i] > hmax) hmax = rem.v[i];
            } else {
                vpush(&left, rem.v[i]);
            }
        }
        Seg *same = NULL;
        for (uint32_t i = 0; i < S->nseg && !same; i++)
            if (S->seg[i].delta == best_d && !S->seg[i].limit) same = &S->seg[i];
        if (same) {
            if (hmin < same->lo) same->lo = hmin;
            if (hmax + 4 > same->hi) same->hi = hmax + 4;
            S->segver++;
            if (same->how == CS_HOW_CALLS) same->how_n += score;
        } else {
            Seg *s = add_seg(S, hmin, hmin + best_d, 0, CS_HOW_CALLS);
            s->how_n = score;
            s->hi = hmax + 4;
            S->segver++;
        }
        found++;
        vfree(&rem);
        rem = left;
    }
    cs_release(own);
    vfree(&rem);
    return found;
}

static void update_hulls(Scan *S) {
    Vec keys = sorted_starts(S);
    for (uint32_t si = 0; si < S->nseg; si++) {
        Seg *seg = &S->seg[si];
        int have = 0;
        uint32_t lo = 0, hi = 0;
        for (uint32_t k = 0; k < keys.n; k++) {
            if (segment_of(S, keys.v[k]) != (int)si) continue;
            Func *f = get_func(S, keys.v[k]);
            if (!have || f->start < lo) lo = f->start;
            if (!have || f->end > hi) hi = f->end;
            have = 1;
        }
        if (have) {
            if (lo < seg->lo) seg->lo = lo, S->segver++;
            if (hi > seg->hi) seg->hi = hi, S->segver++;
        }
    }
    vfree(&keys);
}

static int inside(const Span *sp, uint32_t n, uint32_t v) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (sp[m].start <= v) lo = m + 1;
        else hi = m;
    }
    return lo > 0 && sp[lo - 1].start <= v && v < sp[lo - 1].end;
}

static int in_hull(Scan *S, uint32_t v) {
    int s = segment_of(S, v);
    return s >= 0 && S->seg[s].lo <= v && v < S->seg[s].hi;
}

static int start_like(Scan *S, uint32_t v, uint32_t lo) {
    i64 p = (i64)v - 4, w = NONE;
    int k;
    for (k = 0; k < MAX_PAD; k++) {
        w = word(S, p);
        if (w == NONE || p < lo) return p < lo;
        if (w) break;
        p -= 4;
    }
    if (k == MAX_PAD) return 0;
    return (ends_block(w) && (k > 0 || w == ERET)) || ends_block(word(S, p - 4));  // v is no jump's delay slot
}

static const Map *table_targets(Scan *S) {
    if (S->tt_count != S->funcs.n) {
        map_clear(&S->tt);
        for (uint32_t i = 0; i < S->funcs.cap; i++) {
            if (S->funcs.st[i] != 1) continue;
            const Func *f = S->fpool[S->funcs.val[i]];
            for (uint32_t t = 0; t < f->ntabs; t++)
                for (uint32_t j = 0; j < f->tabs[t].targets.n; j++) map_add(&S->tt, f->tabs[t].targets.v[j]);
        }
        S->tt_count = S->funcs.n;
    }
    return &S->tt;
}

typedef struct { uint32_t v, kind; } Seed;

static int cmp_hull(const void *a, const void *b) {
    const uint32_t *x = a, *y = b;
    if (x[0] != y[0]) return x[0] < y[0] ? -1 : 1;
    return x[1] < y[1] ? -1 : x[1] > y[1];
}

static Seed *pointer_seeds(Scan *S, uint32_t *nout) {
    uint32_t nsp;
    Span *sp = extent_index(S, &nsp);
    uint32_t *hulls = cs_alloc((2 * S->nseg + 2) * 4), *his = cs_alloc((S->nseg + 1) * 4), nh = 0;
    for (uint32_t i = 0; i < S->nseg; i++)
        if (!S->seg[i].real) hulls[2 * nh] = S->seg[i].lo, hulls[2 * nh + 1] = S->seg[i].hi, nh++;
    qsort(hulls, nh, 8, cmp_hull);
    for (uint32_t i = 0; i < nh; i++) his[i] = hulls[2 * i], ptr_span(S, hulls[2 * i], hulls[2 * i + 1]);
    Vec data = {0}, code = {0};
    i64 hi = -1;  // ptr_words and the hulls are both sorted: walk them together (the last hull with lo <= w)
    for (uint32_t k = 0; k < S->ptr_words.n; k++) {
        uint32_t w = S->ptr_words.v[k];
        while (hi + 1 < nh && his[hi + 1] <= w) hi++;
        if (hi >= 0 && w < hulls[2 * hi + 1] && !map_has(&S->funcs, w)) vpush(&data, w);
    }
    for (uint32_t i = 0; i < S->novl; i++) {
        const Seg *o = &S->seg[S->ovl[i]];
        uint32_t lo = ov_real(o->lo), hi2 = lo + (o->hi - o->lo);
        ptr_span(S, lo, hi2);
        for (i64 k = floor_idx(S->ptr_words.v, S->ptr_words.n, lo); k < S->ptr_words.n; k++) {
            if (k < 0) continue;
            uint32_t w = S->ptr_words.v[k];
            if (w >= hi2) break;
            if (w >= lo && !map_has(&S->funcs, ov_key(o, w))) vpush(&data, ov_key(o, w));
        }
    }
    if (S->novl) vsortu(&data);
    for (uint32_t fi = 0; fi < S->funcs.cap; fi++) {
        if (S->funcs.st[fi] != 1) continue;
        const Func *f = S->fpool[S->funcs.val[fi]];
        i64 mi = f->memo_gen ? map_get(&S->memo_of, f->start) : NONE;
        Memo *m = mi != NONE && S->memo[mi].gen == f->memo_gen ? &S->memo[mi] : NULL;
        Vec *pv = m ? &m->pv : &pv_scratch;  // f's lui/addiu values depend only on its seen pcs, through the ins cache
        if (!m || m->pv_gen != m->gen) {
            pv->n = 0;
            uint32_t lui[32], have = 0;
            FOR_SEEN(f, pc) {
                Ins i = ins(S, pc);
                int c = I_CLASS(i.info), rs = (i.raw >> 21) & 31, rt = (i.raw >> 16) & 31;
                if (c == C_LUI) {
                    lui[rt] = (i.raw & 0xFFFF) << 16, have |= 1u << rt;
                    continue;
                }
                if ((c == C_ADDIU || c == C_ORI) && (have >> rs & 1))
                    vpush(pv, lui[rs] + (c == C_ADDIU ? (uint32_t)simm(i.raw) : (i.raw & 0xFFFF)));
                if (i.info & I_MODRT) have &= ~(1u << rt);
                if (i.info & I_MODRD) have &= ~(1u << ((i.raw >> 11) & 31));
            }
            if (m) m->pv_gen = m->gen;
        }
        int ctx = S->novl ? segment_of(S, f->start) : -1;
        for (uint32_t k = 0; k < pv->n; k++) {
            uint32_t ks[OV_MAX], nk = resolve(S, ctx, pv->v[k], ks);
            for (uint32_t j = 0; j < nk; j++)
                if (!map_has(&S->funcs, ks[j]) && in_hull(S, ks[j])) vpush(&code, ks[j]);
        }
    }
    vsortu(&code);  // data is already sorted and unique: it is filtered from ptr_words in order
    const Map *tt = table_targets(S);
    Map out = {0};
    Seed *seeds = cs_alloc((code.n + data.n + 1) * sizeof *seeds);
    uint32_t n = 0;
    const Vec *lists[2] = {&code, &data};
    const uint32_t kinds[2] = {CS_ORIG_LUI, CS_ORIG_DATA};
    for (int l = 0; l < 2; l++)
        for (uint32_t k = 0; k < lists[l]->n; k++) {
            uint32_t v = lists[l]->v[k];
            int s = segment_of(S, v);
            if (s >= 0 && !map_has(&out, v) && !inside(sp, nsp, v) && !map_has(tt, v) &&
                start_like(S, v, S->seg[s].lo)) {
                map_add(&out, v);
                seeds[n].v = v, seeds[n++].kind = kinds[l];
            }
        }
    map_free(&out), vfree(&code), vfree(&data);
    cs_release(hulls), cs_release(his), cs_release(sp);
    *nout = n;
    return seeds;
}

static int epilogue_like(Scan *S, uint32_t p) {
    for (int k = 0; k < 16; k++) {
        i64 w = word(S, (i64)p + 4 * k);
        if (w == NONE || w == JR_RA || ((uint32_t)w >> 16) == 0xAFBF) return 0;
        if (((uint32_t)w >> 16) == 0x8FBF) return 1;
        if (((uint32_t)w >> 16) == 0x27BD) return (w & 0x8000) == 0;
    }
    return 0;
}

static i64 next_start_after(Scan *S, uint32_t v) {
    const Vec *s = sorted_view(S);
    uint32_t lo = 0, hi = s->n;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2;
        if (s->v[m] < v) lo = m + 1;
        else hi = m;
    }
    return lo < s->n ? (i64)s->v[lo] : NONE;
}

static int dup_return(Scan *S, uint32_t end) { return word(S, end) == JR_RA && word(S, (i64)end + 4) == 0; }
static int is_bare_return(Scan *S, const Func *f) { return f->end - f->start == 8 && dup_return(S, f->start); }

static uint32_t merge_dups(Scan *S) {
    Vec keys = sorted_starts(S);
    uint32_t n = keys.n;
    Func **order = cs_alloc((n + 1) * sizeof *order);
    for (uint32_t i = 0; i < n; i++) order[i] = get_func(S, keys.v[i]);
    Vec real_starts = {0}, followed = {0};
    for (uint32_t i = 0; i < n; i++) {
        Func *f = order[i];
        if (is_bare_return(S, f)) continue;
        int fl = dup_return(S, f->end);
        for (uint32_t p = f->start; p < f->end && !fl; p += 4) fl = word(S, p) == B_NEXT && word(S, (i64)p + 4) == 0;
        vpush(&real_starts, f->start);
        vpush(&followed, fl);
    }
    uint32_t merged = 0, i = 0;
    Func **run = cs_alloc((n + 1) * sizeof *run);
    while (i < n) {
        Func *a = order[i];
        uint32_t nrun = 0;
        while (i + 1 + nrun < n && is_bare_return(S, order[i + 1 + nrun]) &&
               order[i + 1 + nrun]->start == (nrun ? run[nrun - 1] : a)->end) {
            run[nrun] = order[i + 1 + nrun];
            nrun++;
        }
        i += 1 + nrun;
        if (!nrun) continue;
        uint32_t lo = 0, hi = real_starts.n;
        while (lo < hi) {
            uint32_t m = (lo + hi) / 2;
            if (real_starts.v[m] < a->start) lo = m + 1;
            else hi = m;
        }
        uint32_t j = lo, sum = 0, len = 0;
        for (uint32_t k = j >= 8 ? j - 8 : 0; k < j; k++) sum += followed.v[k], len++;
        for (uint32_t k = j + 1; k < j + 9 && k < followed.n; k++) sum += followed.v[k], len++;
        if (sum * 2 <= len) continue;
        int framed = word(S, (i64)a->end - 16) == B_NEXT;
        for (uint32_t p = a->start; p < a->end && !framed; p += 4) {
            i64 w = word(S, p);
            framed = w != NONE && ((uint32_t)w >> 16) == 0x27BD && (w & 0x8000);
        }
        Func *owner = a;
        int bare = is_bare_return(S, a);
        int quota = framed || bare ? 0 : 2 - (int)(nrun % 2);
        if (bare) quota = 1;
        for (uint32_t r = 0; r < nrun; r++) {
            Func *b = run[r];
            if (bits_has(&S->strong, b->start) || quota == 0) {
                owner = b, quota = 1;
                continue;
            }
            unlink_func(S, b->start, 1);  // freed below: order[] still points at it
            mark_del(S, &S->starts, b->start);
            own(S, owner);
            dead_put(S, owner->start, b->end);
            owner->end = b->end;
            quota--;
            merged++;
        }
    }
    for (uint32_t k = 0; k < n; k++)
        if (get_func(S, order[k]->start) != order[k]) func_free(order[k]);
    vfree(&real_starts), vfree(&followed), vfree(&keys);
    cs_release(order), cs_release(run);
    return merged;
}

static uint32_t skip_tail(Scan *S, uint32_t end, i64 hi) {
    i64 p = end;
    while (p < hi && word(S, p) == 0 && p - end < 4 * MAX_PAD) p += 4;
    if (p >= hi || !epilogue_like(S, (uint32_t)p)) return end;
    Func *t = explore(S, (uint32_t)p);
    map_del(&S->bad, (uint32_t)p);
    uint32_t r = t && t->end <= hi ? t->end : end;
    func_free(t);
    return r;
}

static int tiles(Scan *S, Func *f) {
    i64 nxt = next_start_after(S, f->end);
    if (nxt == NONE) return 0;
    uint32_t end = skip_tail(S, f->end, nxt);
    int zero = 1;
    for (i64 p = end; p < nxt && zero; p += 4) zero = word(S, p) == 0;
    if (zero && nxt - end <= 4 * MAX_PAD) {
        if (end != f->end) own(S, f), dead_put(S, f->start, end), f->end = end;
        return 1;
    }
    return 0;
}

static int epilogue_like_rom(Scan *S, uint32_t v, uint32_t si) {
    uint32_t saved = S->seg[si].lo;
    if (v < S->seg[si].lo) S->seg[si].lo = v, S->segver++;
    int r = epilogue_like(S, v);
    if (S->seg[si].lo != saved) S->seg[si].lo = saved, S->segver++;
    return r;
}

static Vec back_candidates(Scan *S, uint32_t si, uint32_t lo) {
    Vec out = {0};
    const Seg *seg = &S->seg[si];
    i64 delta = seg->delta;
    for (i64 q = (i64)lo - 8; q > (i64)lo - 0x4000; q -= 4) {
        i64 r = q + delta;
        int ok = r >= 0;
        for (uint32_t i = 0; i < S->nseg && ok; i++)
            if (i != si) ok = !(S->seg[i].lo + S->seg[i].delta <= r && r < S->seg[i].hi + S->seg[i].delta);
        if (!ok) break;
        if (rom_word(S, q + delta) == JR_RA) {
            i64 c = q + 8;
            while (c < lo && rom_word(S, c + delta) == 0) c += 4;
            if (c < lo) vpush(&out, (uint32_t)c);
        }
    }
    Map near = {0};
    Vec nv = {0};
    i64 bot = (i64)lo - 0x4000;
    ptr_span(S, bot, lo);
    const Map *sets[2] = {&S->unplaced, &S->unplaceable};
    for (int k = 0; k < 2; k++)
        for (uint32_t i = 0; i < sets[k]->cap; i++) {
            uint32_t t = sets[k]->key[i];
            if (sets[k]->st[i] == 1 && bot <= t && t < lo && !map_has(&near, t)) map_add(&near, t), vpush(&nv, t);
        }
    for (uint32_t i = (uint32_t)(floor_idx(S->ptr_words.v, S->ptr_words.n, bot > 0 ? (uint32_t)bot - 1 : 0) + 1);
         i < S->ptr_words.n && S->ptr_words.v[i] < lo; i++) {
        uint32_t t = S->ptr_words.v[i];
        if (bot <= t && !map_has(&near, t)) map_add(&near, t), vpush(&nv, t);
    }
    vsortu(&nv);
    for (uint32_t k = nv.n; k-- > 0;) {
        uint32_t t = nv.v[k];
        i64 r = t + delta;
        int ok = r >= 0;
        for (uint32_t i = 0; i < S->nseg && ok; i++)
            if (i != si) ok = !(S->seg[i].lo + S->seg[i].delta <= r && r < S->seg[i].hi + S->seg[i].delta);
        if (ok && !(t & 3)) vpush(&out, t);
    }
    map_free(&near), vfree(&nv);
    return out;
}

static uint32_t extend_back(Scan *S, uint32_t si) {
    uint32_t added = 0;
    for (;;) {
        uint32_t lo = S->seg[si].lo;
        Func *got = NULL;
        Vec cands = back_candidates(S, si, lo);
        for (uint32_t k = 0; k < cands.n; k++) {
            uint32_t c = cands.v[k];
            if (epilogue_like_rom(S, c, si)) continue;
            S->seg[si].lo = c, S->segver++;
            mark_add(S, &S->starts, c);
            Func *f = explore(S, c);
            if (f && !f->unres.n && tiles(S, f)) {
                got = f;
                break;
            }
            func_free(f);
            mark_del(S, &S->starts, c);
            map_del(&S->bad, c);
            S->seg[si].lo = lo, S->segver++;
        }
        vfree(&cands);
        if (!got) return added;
        put_func(S, got->start, got);
        map_put(&S->origin, got->start, CS_ORIG_TILING);
        absorb(S, got);
        added++;
    }
}

static uint32_t sweep(Scan *S, Vec *starts) {
    uint32_t added = 0;
    for (uint32_t si = 0; si < S->nseg; si++) {
        Vec all = sorted_starts(S), st = {0};
        for (uint32_t k = 0; k < all.n; k++)
            if (segment_of(S, all.v[k]) == (int)si) vpush(&st, all.v[k]);
        vfree(&all);
        if (!st.n) {
            vfree(&st);
            continue;
        }
        i64 wlo, whi;
        window(&S->seg[si], &wlo, &whi);
        for (uint32_t b = 0; b < st.n; b++) {
            uint32_t prev = st.v[b];
            i64 hi = b + 1 < st.n ? (i64)st.v[b + 1] : whi;
            Func *pf = get_func(S, prev);
            uint32_t end = skip_tail(S, pf->end, hi);
            if (end != pf->end) own(S, pf), dead_put(S, prev, end), pf->end = end;
            i64 p = end;
            while (p < hi) {
                i64 w = word(S, p);
                if (w == NONE) break;
                if (w == 0) {
                    p += 4;
                    continue;
                }
                mark_add(S, &S->starts, (uint32_t)p);
                Func *f = explore(S, (uint32_t)p);
                int ret = 0;
                if (f)
                    FOR_SEEN(f, x) {
                        i64 xw = word_seen(S, x);
                        if (xw == JR_RA || xw == SELF_LOOP) {
                            ret = 1;
                            break;
                        }
                    }
                int loop = f && !ret && f->end <= hi && skip_tail(S, f->end, hi) != f->end;  // infinite loop + dead epilogue
                if (!f || f->end > hi || !f->nseen || f->maxseen < p + 4 || f->unres.n || !(ret || loop)) {
                    func_free(f);
                    mark_del(S, &S->starts, (uint32_t)p);
                    map_del(&S->bad, (uint32_t)p);
                    break;
                }
                put_func(S, (uint32_t)p, f);
                map_put(&S->origin, (uint32_t)p, CS_ORIG_SWEEP);
                vpush(starts, (uint32_t)p);
                added++;
                uint32_t e2 = skip_tail(S, f->end, hi);
                if (e2 != f->end) own(S, f), dead_put(S, (uint32_t)p, e2), f->end = e2;
                p = f->end;
            }
        }
        vfree(&st);
    }
    return added;
}

// The public API
static void *zalloc(size_t n) {
    void *p = cs_alloc(n);
    memset(p, 0, n);
    return p;
}

Scan *cs_new(uint32_t rom_size, cs_page_fn page, void *ctx) {
    if (rom_size < BOOT_ROM + 4) cs_die("scan: ROM too small");
    Scan *S = zalloc(sizeof *S);
    S->size = rom_size;
    S->nwords = rom_size / 4;
    S->page = page, S->ctx = ctx;
    S->rom_pages = zalloc(((S->nwords + 1023) / 1024 + 1) * sizeof *S->rom_pages);
    S->tt_count = -1;
    S->starts.lastidx = S->strong.lastidx = S->table_entries.lastidx = NONE;
    return S;
}

static void pend_done(Scan *S, uint32_t k) {
    vpush(&S->rom_starts, S->pend[k].p);
    S->pend[k] = S->pend[--S->npend];
}

void cs_feed(Scan *S, const uint32_t *w, uint32_t n) {
    for (uint32_t i = 0; i < n; i++, S->fed++) {
        uint32_t x = w[i], off = 4 * S->fed;
        for (uint32_t k = 0; k < S->npend;) {
            if (S->pend[k].p != off) {
                k++;
                continue;
            }
            if (x != 0) {
                pend_done(S, k);
                continue;
            }
            S->pend[k].p += 4;
            if (++S->pend[k].steps == MAX_PAD) {
                pend_done(S, k);
                continue;
            }
            k++;
        }
        if (x == JR_RA) {
            if (S->npend == 16) cs_die("scan: too many pending jr ra starts");
            S->pend[S->npend].p = off + 8, S->pend[S->npend++].steps = 0;
        }
        if (!(x & 3) && (x >> 31)) {
            vpush(&S->ptr_words, x);
            if (S->ptr_words.n == S->ptr_words.cap && S->ptr_words.n >= 65536) {
                vsortu(&S->ptr_words);  // compact; grow too unless that freed half, so this stays amortized
                if (2 * S->ptr_words.n > S->ptr_words.cap)
                    S->ptr_words.v = cs_realloc(S->ptr_words.v, (S->ptr_words.cap *= 2) * 4);
            }
        }
    }
}

void cs_set_tables(Scan *S, const uint32_t *starts, uint32_t nstarts, const uint32_t *ptrs, uint32_t nptrs,
                   uint32_t ptr_lo, uint32_t ptr_hi) {
    for (uint32_t i = 0; i < nstarts; i++) vpush(&S->rom_starts, starts[i]);
    for (uint32_t i = 0; i < nptrs; i++) vpush(&S->ptr_words, ptrs[i]);
    S->fed = S->nwords, S->npend = 0;
    S->ptr_lo = ptr_lo, S->ptr_hi = ptr_hi, S->ptr_ranged = 1;
}

static int cmp_tab(const void *a, const void *b) {
    const CsTab *x = a, *y = b;
    return x->jr < y->jr ? -1 : x->jr > y->jr;
}

static void build_result(Scan *S) {
    CsResult *R = &S->res;
    R->entry = S->entry;
    R->nseg = S->nseg;
    R->seg = zalloc((S->nseg + 1) * sizeof *R->seg);
    for (uint32_t i = 0; i < S->nseg; i++) {
        const Seg *s = &S->seg[i];
        if (s->real) R->seg[i] = (CsSeg){s->vram, (uint32_t)(s->vram + s->delta), (uint32_t)(s->limit - s->vram), s->how, 0};
        else R->seg[i] = (CsSeg){s->lo, (uint32_t)(s->lo + s->delta), s->hi - s->lo, s->how, s->how_n};
    }
    Vec keys = sorted_starts(S);
    R->nfunc = keys.n;
    R->func = zalloc((keys.n + 1) * sizeof *R->func);
    uint32_t ntab = 0, nwords = 0;  // the result owns copies, so the scan's working state can go first
    for (uint32_t k = 0; k < keys.n; k++) {
        const Func *f = get_func(S, keys.v[k]);
        ntab += f->ntabs, nwords += f->unres.n;
        for (uint32_t t = 0; t < f->ntabs; t++) nwords += f->tabs[t].targets.n;
    }
    R->tab = zalloc((ntab + 1) * sizeof *R->tab);
    uint32_t *pool = R->pool = zalloc((nwords + 1) * 4);
    for (uint32_t k = 0; k < keys.n; k++) {
        const Func *f = get_func(S, keys.v[k]);
        i64 o = map_get(&S->origin, f->start);
        memcpy(pool, f->unres.v, f->unres.n * 4);
        R->func[k] = (CsFunc){f->start, f->end, o == NONE ? CS_ORIG_CALL : (uint32_t)o, f->unres.n, pool};
        pool += f->unres.n;
        uint32_t first = R->ntab;
        for (uint32_t t = 0; t < f->ntabs; t++) {
            memcpy(pool, f->tabs[t].targets.v, f->tabs[t].targets.n * 4);
            R->tab[R->ntab++] = (CsTab){f->tabs[t].jr, f->tabs[t].table, f->tabs[t].targets.n, pool};
            pool += f->tabs[t].targets.n;
        }
        qsort(R->tab + first, R->ntab - first, sizeof *R->tab, cmp_tab);
    }
    vfree(&keys);
    Vec u = {0};
    const Map *sets[2] = {&S->unplaced, &S->unplaceable};
    for (int k = 0; k < 2; k++)
        for (uint32_t i = 0; i < sets[k]->cap; i++)
            if (sets[k]->st[i] == 1 && !map_has(&S->funcs, sets[k]->key[i])) vpush(&u, sets[k]->key[i]);
    vsortu(&u);
    R->nunplaced = u.n;
    R->unplaced = u.v;
}

// From each IPL3's stage 2; 6102's never writes osCicId, so it keeps the 2 the boot image always had
static const struct { uint64_t sum; CsIpl3 v; } ipl3_tab[] = {
    { 0xD6497E414Bull, { 0x100000, 6103, 0xFFFFFFFFu, 6103, 0 } },     // 6103
    { 0xD6D5BE5580ull, { 0x200000, 6104, 0xFFFFFFFFu, 6104, 0x85 } },  // 6106
    { 0x11A49F60E96ull, { 0, 6105, 0xA4002000u, 0xA4002000u, 0 } },  // 6105
};
static const CsIpl3 ipl3_6102 = { 0, 2, 0, 0, 0 };

uint32_t cs_boot_load(const uint32_t *page0, const CsIpl3 **ipl3) {
    uint64_t sum = 0;  // IPL3's words, ROM 0x40-0x1000
    for (uint32_t i = 0x10; i < 0x400; i++) sum += page0[i];
    const CsIpl3 *v = &ipl3_6102;
    for (uint32_t i = 0; i < sizeof ipl3_tab / sizeof ipl3_tab[0]; i++)
        if (ipl3_tab[i].sum == sum) v = &ipl3_tab[i].v;
    if (ipl3) *ipl3 = v;
    return page0[2] - v->shift;
}

// The boot segment ends where entry.s starts clearing BSS ($t0 at its first sw $zero, 0($t0)); else IPL3's 1 MB
static uint32_t boot_size(Scan *S) {
    uint32_t hs = S->size - BOOT_ROM < BOOT_SIZE ? S->size - BOOT_ROM : BOOT_SIZE, t0 = 0;
    int have = 0;
    for (uint32_t k = 0; k < 8; k++) {
        uint32_t w = rom_word_at(S, BOOT_ROM / 4 + k);
        if (w == 0xAD000000u) return have && t0 > S->entry && t0 - S->entry < hs ? t0 - S->entry : hs;
        if (w >> 16 == 0x3C08) t0 = w << 16, have = 1;
        else if (w >> 16 == 0x2508) t0 += (uint32_t)(int32_t)(int16_t)w;
        else if (w >> 16 == 0x3508) t0 |= w & 0xFFFF;
    }
    return hs;
}

// The pointer seeds not yet tried, into nw
static void new_pointer_seeds(Scan *S, Vec *nw) {
    uint32_t nseeds;
    Seed *seeds = pointer_seeds(S, &nseeds);
    nw->n = 0;
    for (uint32_t k = 0; k < nseeds; k++) {
        uint32_t v = seeds[k].v;
        if (map_has(&S->funcs, v) || map_has(&S->tried, v)) continue;
        vpush(nw, v);
        if (!map_has(&S->origin, v)) map_put(&S->origin, v, seeds[k].kind);
    }
    for (uint32_t k = 0; k < nw->n; k++) map_add(&S->tried, nw->v[k]);
    cs_release(seeds);
}

static void requeue(Scan *S, Vec *queue) {
    queue->n = 0;
    Vec keys = sorted_starts(S);
    for (uint32_t k = 0; k < keys.n; k++) {
        const Func *f = get_func(S, keys.v[k]);
        if (bits_has(&S->strong, f->start)) push_union(queue, f);
        else
            for (uint32_t i = 0; i < f->calls.n; i++) vpush(queue, f->calls.v[i]);
    }
    vfree(&keys);
}

static void finish(Scan *S);

const CsResult *cs_run(Scan *S) {
    if (S->fed != S->nwords) cs_die("scan: the ROM was not fed in full");
    while (S->npend) pend_done(S, 0);
    rom_starts_done(S);
    rom_word_at(S, 0);
    S->entry = cs_boot_load(S->rom_pages[0], NULL);
    uint32_t hs = boot_size(S);
    add_seg(S, S->entry, BOOT_ROM, hs, CS_HOW_HEADER);
    RabbitizerConfig_Cfg.pseudos.enablePseudos = false;
    Vec queue = {0}, nw = {0};
    vpush(&queue, S->entry);
    uint32_t origin = CS_ORIG_ENTRY;
    for (;;) {
        run_closure(S, &nw, CS_ORIG_POINTER, 0);
        run_closure(S, &queue, origin, 1);
        uint32_t placed = S->unplaced.n ? table_segments(S) : 0;
        placed += icache_segments(S);
        if (S->unplaced.n) placed += call_segments(S);
        if (S->unplaced.n) placed += place_segments(S);
        update_hulls(S);
        uint32_t tiled = 0;
        for (uint32_t si = 0; si < S->nseg; si++)
            if (!S->seg[si].limit) tiled += extend_back(S, si);
        if (tiled) {
            refine(S);
            update_hulls(S);
        }
        new_pointer_seeds(S, &nw);
        S->res.rounds++;
        cs_log("  round: %u functions, %u segments placed, %u tiled, %u new pointer seeds\n", (unsigned)S->funcs.n,
               (unsigned)placed, (unsigned)tiled, (unsigned)nw.n);
        if (!placed && !nw.n && !tiled) break;
        requeue(S, &queue);
        origin = CS_ORIG_CALL;
    }
    vfree(&queue), vfree(&nw);
    finish(S);
    return &S->res;
}

// Run-time code (harness/rgn.c): ROM offset 0 is vram and the whole "ROM" is one segment; targets outside it stay
// unplaced, since the code they reach is translated already
const CsResult *cs_run_region(Scan *S, uint32_t vram, const uint32_t *seeds, uint32_t nseeds) {
    if (S->fed != S->nwords) cs_die("scan: the region was not fed in full");
    while (S->npend) pend_done(S, 0);
    rom_starts_done(S);
    S->entry = nseeds ? seeds[0] : vram;
    add_seg(S, vram, 0, S->size, CS_HOW_ICACHE);
    RabbitizerConfig_Cfg.pseudos.enablePseudos = false;
    Vec queue = {0}, nw = {0};
    for (uint32_t i = 0; i < nseeds; i++) vpush(&queue, seeds[i]);
    uint32_t origin = CS_ORIG_ENTRY;
    for (;;) {
        run_closure(S, &nw, CS_ORIG_POINTER, 0);
        run_closure(S, &queue, origin, 1);
        update_hulls(S);
        new_pointer_seeds(S, &nw);
        S->res.rounds++;
        cs_log("  region round: %u functions, %u new pointer seeds\n", (unsigned)S->funcs.n, (unsigned)nw.n);
        if (!nw.n) break;
        requeue(S, &queue);
        origin = CS_ORIG_CALL;
    }
    vfree(&queue), vfree(&nw);
    finish(S);
    return &S->res;
}

static void finish(Scan *S) {
    for (;;) {  // code only the sweep reached still calls functions nothing else reaches
        Vec added = {0}, calls = {0};
        sweep(S, &added);
        for (uint32_t k = 0; k < added.n; k++) {
            const Func *f = get_func(S, added.v[k]);
            for (uint32_t i = 0; f && i < f->calls.n; i++)
                if (!map_has(&S->funcs, f->calls.v[i])) vpush(&calls, f->calls.v[i]);
        }
        vfree(&added);
        vsortu(&calls);
        if (!calls.n) {
            vfree(&calls);
            break;
        }
        run_closure(S, &calls, CS_ORIG_CALL, 1);
        vfree(&calls);
        update_hulls(S);
    }
    merge_dups(S);
    refine(S);
    update_hulls(S);
    build_result(S);
}


void cs_drop_work(Scan *S) {
    if (!S || S->dropped) return;
    S->dropped = 1;
    for (uint32_t i = 0; i < S->nfpool; i++) func_free(S->fpool[i]);
    cs_release(S->fpool);
    vfree(&S->ffree);
    Map *maps[] = {&S->funcs, &S->bad, &S->dead_end, &S->origin, &S->unplaced, &S->unplaceable, &S->tried,
                   &S->cache_pages, &S->rom_starts_set, &S->tt};
    for (size_t i = 0; i < sizeof maps / sizeof *maps; i++) map_free(maps[i]);
    bits_free(&S->starts), bits_free(&S->strong), bits_free(&S->table_entries);
    vfree(&S->sorted);
    for (uint32_t i = 0; i < S->npages; i++) cs_release(S->pages[i]);
    cs_release(S->pages);
    vfree(&S->rom_starts), vfree(&S->ptr_words);
    for (uint32_t i = 0; i < S->nmemo; i++)
        func_free(S->memo[i].f), vfree(&S->memo[i].deps), vfree(&S->memo[i].ic_calls), vfree(&S->memo[i].pv);
    cs_release(S->memo), cs_release(S->blk), cs_release(S->moved);
    map_free(&S->blk_pages), map_free(&S->memo_of);
    cs_release(S->rom_pages);
    cs_release(S->seg);
    S->fpool = NULL, S->memo = NULL, S->pages = NULL, S->rom_pages = NULL, S->seg = NULL;
}

void cs_free(Scan *S) {
    if (!S) return;
    cs_drop_work(S);
    cs_release(S->res.seg), cs_release(S->res.func), cs_release(S->res.tab), cs_release(S->res.unplaced);
    cs_release(S->res.pool);
    cs_release(S);
    vfree(&ic_scratch), vfree(&pv_scratch);
}
