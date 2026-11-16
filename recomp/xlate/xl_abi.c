// Which functions take or keep registers outside the call ABI (M.raw, M.keep): they are called with every register
#include "xl_int.h"

// What a call hands the callee: the argument and saved registers before_call stores, gp/sp/fp/ra; FPU likewise
#define G_PASSED (BIT(0) | RANGE(4, 7) | RANGE(16, 23) | RANGE(28, 31))
#define F_PASSED (RANGE(12, 15) | RANGE(20, 31))

static void decode(RabbitizerInstruction *ri, uint32_t raw, uint32_t vram) {
    RabbitizerInstruction_init(ri, raw, vram);
    RabbitizerInstruction_processUniqueId(ri);
}

static void reg_effects(const RabbitizerInstruction *ri, uint32_t raw, uint64_t *gr, uint64_t *fr, uint64_t *gw,
                        uint64_t *fw) {
    const RabbitizerInstrDescriptor *d = ri->descriptor;
    int rs = (raw >> 21) & 31, rt = (raw >> 16) & 31, rd = (raw >> 11) & 31, fd = (raw >> 6) & 31;
    int dbl = (raw >> 26) == 0x11 && ((raw >> 21) & 31) == 0x11;
    int pair = dbl || (raw >> 26) == 0x35 || (raw >> 26) == 0x3D;  // .d ops, ldc1, sdc1
    *gr = (RabbitizerInstrDescriptor_readsRs(d) ? BIT(rs) : 0) | (RabbitizerInstrDescriptor_readsRt(d) ? BIT(rt) : 0) |
          (RabbitizerInstrDescriptor_readsRd(d) ? BIT(rd) : 0);
    *gw = (RabbitizerInstrDescriptor_modifiesRs(d) ? BIT(rs) : 0) |
          (RabbitizerInstrDescriptor_modifiesRt(d) ? BIT(rt) : 0) |
          (RabbitizerInstrDescriptor_modifiesRd(d) ? BIT(rd) : 0) | (RabbitizerInstrDescriptor_doesLink(d) ? BIT(31) : 0);
    *fr = (RabbitizerInstrDescriptor_readsFs(d) ? BIT(rd) : 0) | (RabbitizerInstrDescriptor_readsFt(d) ? BIT(rt) : 0) |
          (RabbitizerInstrDescriptor_readsFd(d) ? BIT(fd) : 0);
    uint64_t w = (RabbitizerInstrDescriptor_modifiesFs(d) ? BIT(rd) : 0) |
                 (RabbitizerInstrDescriptor_modifiesFt(d) ? BIT(rt) : 0) |
                 (RabbitizerInstrDescriptor_modifiesFd(d) ? BIT(fd) : 0);
    *fw = pair ? w | (w << 1) : w;
}

static uint64_t undef_g, undef_f;  // what reads_undefined's hit read
static uint64_t *need_g, *need_f;   // per raw (2) function: what it reads that no call passes; NULL: calls read nothing
static uint64_t acc_g, acc_f;       // reads_undefined's every undefined read, when accumulating

// What a jal at v makes its raw callee read
static void callee_reads(uint32_t v, uint32_t raw, uint64_t *gr, uint64_t *fr) {
    *gr = *fr = 0;
    if (!need_g || raw >> 26 != 3) return;
    int many;
    int k = find_u32(M.start, M.nfunc, ov_resolve(v, j_target(v, raw), &many));
    if (k >= 0 && !many && M.raw[k] == 2) *gr = need_g[k], *fr = need_f[k];
}

// The first instruction on some path that reads a register before writing it, given what is defined at entry and
// after each call; 0 if none. accumulate: go on past each hit, gathering every such read in acc_g/acc_f
static uint32_t reads_undefined(uint32_t lo, uint32_t hi, uint64_t g_entry, uint64_t f_entry, uint64_t g_call,
                                uint64_t f_call, int accumulate) {
    uint32_t n = (hi - lo) / 4;
    if (n < 2) return 0;
    uint64_t *gin = xl_alloc(16 * (size_t)n + 16), *fin = gin + n + 1;
    uint32_t *todo = xl_alloc(4 * (size_t)n + 16), nt = 0;
    uint8_t *seen = xl_alloc(2 * (size_t)n + 2), *queued = seen + n + 1;
    memset(seen, 0, 2 * (size_t)n + 2);
    uint32_t found = 0;
    gin[0] = g_entry, fin[0] = f_entry, seen[0] = queued[0] = 1, todo[nt++] = 0;
#define FLOW(s, g_, f_)                                                                                    \
    do {                                                                                                   \
        uint32_t s_ = (s);                                                                                 \
        if (s_ < n && (!seen[s_] || (gin[s_] & (g_)) != gin[s_] || (fin[s_] & (f_)) != fin[s_])) {          \
            gin[s_] = seen[s_] ? gin[s_] & (g_) : (g_), fin[s_] = seen[s_] ? fin[s_] & (f_) : (f_);          \
            seen[s_] = 1;                                                                                  \
            if (!queued[s_]) queued[s_] = 1, todo[nt++] = s_;                                              \
        }                                                                                                  \
    } while (0)
#define HIT(at, mg_, mf_)                                                     \
    do {                                                                      \
        uint64_t hg_ = (mg_), hf_ = (mf_);                                    \
        if (!found) found = (at), undef_g = hg_, undef_f = hf_;               \
        acc_g |= hg_, acc_f |= hf_;                                           \
    } while (0)
    while (nt && (accumulate || !found)) {
        uint32_t i = todo[--nt], v = lo + 4 * i, raw = word(v);
        queued[i] = 0;
        uint64_t g = gin[i], f = fin[i], gr, fr, gw, fw;
        RabbitizerInstruction ri;
        decode(&ri, raw, v);
        reg_effects(&ri, raw, &gr, &fr, &gw, &fw);
        if ((gr & ~g & ~BIT(0)) || (fr & ~f)) {
            HIT(v, gr & ~g & ~BIT(0), fr & ~f);
            if (!accumulate) break;
            g |= gr, f |= fr;
        }
        g |= gw, f |= fw;
        const RabbitizerInstrDescriptor *d = ri.descriptor;
        int branch = RabbitizerInstrDescriptor_isBranch(d), jump = RabbitizerInstrDescriptor_isJump(d);
        if (!branch && !jump) {
            if (raw != 0x42000018u) FLOW(i + 1, g, f);
            continue;
        }
        uint64_t sg = g, sf = f, sgr, sfr, sgw, sfw;  // the delay slot
        if (i + 1 < n) {
            RabbitizerInstruction slot;
            uint32_t sraw = word(v + 4);
            decode(&slot, sraw, v + 4);
            reg_effects(&slot, sraw, &sgr, &sfr, &sgw, &sfw);
            if ((sgr & ~g & ~BIT(0)) || (sfr & ~f)) {
                HIT(v + 4, sgr & ~g & ~BIT(0), sfr & ~f);
                if (!accumulate) break;
                sg |= sgr, sf |= sfr;
            }
            sg |= sgw, sf |= sfw;
        }
        if (is_getpc(raw)) {
            FLOW(i + 2, sg, sf);
            continue;
        }
        if (RabbitizerInstrDescriptor_doesLink(d)) {
            uint64_t cg, cf;  // a raw callee reads what it takes here: THPS2's table builder passes on its bit buffer
            callee_reads(v, raw, &cg, &cf);
            if ((cg & ~sg & ~BIT(0)) || (cf & ~sf)) {
                HIT(v, cg & ~sg & ~BIT(0), cf & ~sf);
                if (!accumulate) break;
            }
            FLOW(i + 2, g_call, f_call);
            continue;
        }
        int likely = RabbitizerInstrDescriptor_isBranchLikely(d);
        if (branch) {
            uint32_t tg = RabbitizerInstruction_getBranchVramGeneric(&ri);
            if (tg >= lo && tg < hi) FLOW((tg - lo) / 4, sg, sf);
            if ((raw >> 16) != 0x1000) FLOW(i + 2, likely ? g : sg, likely ? f : sf);
            continue;
        }
        if (ri.uniqueId == RABBITIZER_INSTR_ID_cpu_j) {
            int many;
            uint32_t tg = ov_resolve(lo, j_target(v, raw), &many);
            if (tg >= lo && tg < hi) FLOW((tg - lo) / 4, sg, sf);
            continue;
        }
        for (uint32_t k = 0; k < M.ntab; k++)  // jr: its jump table's targets, else it leaves
            if (M.tab[k].jr == v)
                for (uint32_t j = 0; j < M.tab[k].n; j++) {
                    uint32_t tg = rd32(M.tab[k].targets + 4 * j);
                    if (tg >= lo && tg < hi) FLOW((tg - lo) / 4, sg, sf);
                }
    }
#undef HIT
#undef FLOW
    xl_free(gin), xl_free(todo), xl_free(seen);
    return found;
}

// IDO passes a static callee's arguments in temporaries (libultra's _pullSubFrame takes its command pointer in t0)
static int reads_unpassed(uint32_t lo, uint32_t hi) {
    return reads_undefined(lo, hi, G_PASSED, F_PASSED, ALL, ALL, 0) != 0;
}

// IDO keeps temporaries live across a call to a static callee that leaves them alone (naudio's seq in t4)
static uint32_t keeps_unsaved(uint32_t lo, uint32_t hi) {
    uint64_t *g = need_g;
    need_g = NULL;  // raw callees' reads count only for raw_pass
    uint32_t at = reads_undefined(lo, hi, ALL, ALL, BIT(0) | G_RELOAD | BIT(31), F_RELOAD, 0);
    need_g = g;
    return at;
}

// Whether the function at fi jals a raw (2) function
static int calls_raw(uint32_t fi) {
    for (uint32_t v = M.start[fi]; v < M.end[fi]; v += 4) {
        uint64_t g, f;
        callee_reads(v, word(v), &g, &f);
        if (g | f) return 1;
    }
    return 0;
}

// need_g/need_f[fi]: every register fi reads that no call passes, its raw callees' included
static int gather_need(uint32_t fi) {
    acc_g = acc_f = 0;
    reads_undefined(M.start[fi], M.end[fi], G_PASSED, F_PASSED, ALL, ALL, 1);
    int grew = (acc_g & ~need_g[fi]) || (acc_f & ~need_f[fi]);
    need_g[fi] |= acc_g, need_f[fi] |= acc_f;
    return grew;
}

static int low_bit(uint64_t m) {
    int k = 0;
    while (m && !(m & 1)) m >>= 1, k++;
    return k;
}

// M.keep: the function's own calls pass every register both ways
void keep_pass(void) {
    M.keep = xl_alloc(M.nfunc + 1);
    memset(M.keep, 0, M.nfunc + 1);
    uint32_t nkeep = 0;
    for (uint32_t fi = 0; fi < M.nfunc; fi++) {
        int hooked = 0;
        for (uint32_t h = 0; h < M.nhook && !hooked; h++) hooked = M.hook_addr[h] == M.start[fi];
        uint32_t at = hooked ? 0 : keeps_unsaved(M.start[fi], M.end[fi]);
        if (at) {
            M.keep[fi] = 1, nkeep++;
            xl_log("  f_%08X reads registers its calls may clobber: %s%d at %08X\n", (unsigned)M.start[fi],
                   undef_g ? "r" : "f", low_bit(undef_g ? undef_g : undef_f), (unsigned)at);
        }
    }
    if (nkeep) xl_log("  %u functions keep registers across calls: they call with every register\n", nkeep);
}

// M.raw: 1 for a bal target, 2 for a function that reads registers no call passes
static int is_hooked(uint32_t fi) {
    for (uint32_t h = 0; h < M.nhook; h++)
        if (M.hook_addr[h] == M.start[fi]) return 1;
    return 0;
}

void abi_release(void) {
    xl_free(need_g), xl_free(need_f);
    need_g = need_f = NULL;
}

void raw_pass(void) {
    abi_release();
    M.raw = xl_alloc(M.nfunc + 1);
    memset(M.raw, 0, M.nfunc + 1);
    for (uint32_t fi = 0; fi < M.nfunc; fi++)
        for (uint32_t v = M.start[fi]; v < M.end[fi]; v += 4) {
            uint32_t w = word(v);
            int k = is_bal(w) ? find_u32(M.start, M.nfunc, v + 4 + ((int32_t)(int16_t)w << 2)) : -1;
            if (k >= 0) M.raw[k] = 1;
        }
    uint32_t nunpassed = 0;
    for (uint32_t fi = 0; fi < M.nfunc; fi++) {
        if (!is_hooked(fi) && !M.raw[fi] && reads_unpassed(M.start[fi], M.end[fi])) {
            M.raw[fi] = 2, nunpassed++;
            xl_log("  f_%08X takes registers no ABI call passes\n", (unsigned)M.start[fi]);
        }
    }
    need_g = xl_alloc(8 * ((size_t)M.nfunc + 1)), need_f = xl_alloc(8 * ((size_t)M.nfunc + 1));
    memset(need_g, 0, 8 * ((size_t)M.nfunc + 1)), memset(need_f, 0, 8 * ((size_t)M.nfunc + 1));
    for (uint32_t fi = 0; fi < M.nfunc; fi++)
        if (M.raw[fi] == 2) gather_need(fi);
    for (int changed = nunpassed != 0; changed;) {  // a caller that hands a raw callee registers it was given takes them
        changed = 0;
        for (uint32_t fi = 0; fi < M.nfunc; fi++) {
            if (M.raw[fi] == 1 || is_hooked(fi) || !calls_raw(fi)) continue;
            if (!M.raw[fi]) {
                uint32_t at = reads_undefined(M.start[fi], M.end[fi], G_PASSED, F_PASSED, ALL, ALL, 0);
                if (!at) continue;
                M.raw[fi] = 2, nunpassed++, changed = 1;
                xl_log("  f_%08X hands its raw callee %s%d at %08X\n", (unsigned)M.start[fi], undef_g ? "r" : "f",
                       low_bit(undef_g ? undef_g : undef_f), (unsigned)at);
            }
            if (gather_need(fi)) changed = 1;
        }
    }
    if (nunpassed) xl_log("  %u functions read registers a call doesn't pass: called with every register\n", nunpassed);
}

uint32_t xl_raw_pass(const uint8_t *meta, size_t len, uint8_t *raw) {
    load_meta(meta, len);
    init_ops();
    raw_pass();
    for (uint32_t fi = 0; fi < M.nfunc; fi++) raw[fi] = M.raw[fi] != 0;
    return M.nfunc;
}
