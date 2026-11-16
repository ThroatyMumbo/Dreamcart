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

// The first instruction on some path that reads a register before writing it, given what is defined at entry and
// after each call; 0 if none
static uint32_t reads_undefined(uint32_t lo, uint32_t hi, uint64_t g_entry, uint64_t f_entry, uint64_t g_call,
                                uint64_t f_call) {
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
    while (nt && !found) {
        uint32_t i = todo[--nt], v = lo + 4 * i, raw = word(v);
        queued[i] = 0;
        uint64_t g = gin[i], f = fin[i], gr, fr, gw, fw;
        RabbitizerInstruction ri;
        decode(&ri, raw, v);
        reg_effects(&ri, raw, &gr, &fr, &gw, &fw);
        if ((gr & ~g & ~BIT(0)) || (fr & ~f)) { found = v, undef_g = gr & ~g & ~BIT(0), undef_f = fr & ~f; break; }
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
                found = v + 4, undef_g = sgr & ~g & ~BIT(0), undef_f = sfr & ~f;
                break;
            }
            sg |= sgw, sf |= sfw;
        }
        if (is_getpc(raw)) {
            FLOW(i + 2, sg, sf);
            continue;
        }
        if (RabbitizerInstrDescriptor_doesLink(d)) {
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
#undef FLOW
    xl_free(gin), xl_free(todo), xl_free(seen);
    return found;
}

// IDO passes a static callee's arguments in temporaries (libultra's _pullSubFrame takes its command pointer in t0)
static int reads_unpassed(uint32_t lo, uint32_t hi) {
    return reads_undefined(lo, hi, G_PASSED, F_PASSED, ALL, ALL) != 0;
}

// IDO keeps temporaries live across a call to a static callee that leaves them alone (naudio's seq in t4)
static uint32_t keeps_unsaved(uint32_t lo, uint32_t hi) {
    return reads_undefined(lo, hi, ALL, ALL, BIT(0) | G_RELOAD | BIT(31), F_RELOAD);
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
void raw_pass(void) {
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
        int hooked = 0;
        for (uint32_t h = 0; h < M.nhook && !hooked; h++) hooked = M.hook_addr[h] == M.start[fi];
        if (!hooked && !M.raw[fi] && reads_unpassed(M.start[fi], M.end[fi])) {
            M.raw[fi] = 2, nunpassed++;
            xl_log("  f_%08X takes registers no ABI call passes\n", (unsigned)M.start[fi]);
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
