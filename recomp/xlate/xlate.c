// MIPS -> SH-4 machine code and relocations
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xl_int.h"
#include "sh4.h"

#define OPS(X)                                                                                                    \
    X(addu, "addu") X(add, "add") X(or, "or") X(and, "and") X(xor, "xor") X(nor, "nor") X(subu, "subu")          \
    X(sub, "sub") X(slt, "slt") X(sltu, "sltu") X(addiu, "addiu") X(addi, "addi") X(andi, "andi") X(ori, "ori")   \
    X(xori, "xori") X(lui, "lui") X(slti, "slti") X(sltiu, "sltiu") X(sll, "sll") X(srl, "srl") X(sra, "sra")     \
    X(mult, "mult") X(multu, "multu") X(mflo, "mflo") X(mfhi, "mfhi") X(mtlo, "mtlo") X(mthi, "mthi")             \
    X(lw, "lw") X(lh, "lh") X(lhu, "lhu") X(lb, "lb") X(lbu, "lbu") X(sw, "sw") X(sh, "sh") X(sb, "sb")           \
    X(lwc1, "lwc1") X(swc1, "swc1") X(ldc1, "ldc1") X(sdc1, "sdc1") X(mtc1, "mtc1") X(mfc1, "mfc1")               \
    X(add_s, "add.s") X(sub_s, "sub.s") X(mul_s, "mul.s") X(div_s, "div.s") X(mov_s, "mov.s") X(neg_s, "neg.s")   \
    X(cvt_s_w, "cvt.s.w") X(trunc_w_s, "trunc.w.s") X(sqrt_s, "sqrt.s") X(abs_s, "abs.s") X(c_le_s, "c.le.s")     \
    X(c_lt_s, "c.lt.s") X(c_eq_s, "c.eq.s") X(add_d, "add.d") X(sub_d, "sub.d") X(mul_d, "mul.d")                 \
    X(div_d, "div.d") X(cvt_d_s, "cvt.d.s") X(cvt_s_d, "cvt.s.d") X(cvt_d_w, "cvt.d.w") X(trunc_w_d, "trunc.w.d") \
    X(c_lt_d, "c.lt.d") X(c_le_d, "c.le.d") X(c_eq_d, "c.eq.d") X(mov_d, "mov.d") X(neg_d, "neg.d")               \
    X(div, "div") X(divu, "divu") X(sllv, "sllv") X(srlv, "srlv") X(srav, "srav") X(brk, "break")                 \
    X(cache, "cache") X(lwl, "lwl") X(lwr, "lwr") X(swl, "swl") X(swr, "swr") X(ld, "ld") X(sd, "sd")             \
    X(cvt_w_s, "cvt.w.s") X(cfc1, "cfc1") X(ctc1, "ctc1") X(jal, "jal") X(j, "j") X(jr, "jr") X(jalr, "jalr")     \
    X(beq, "beq") X(beql, "beql") X(bne, "bne") X(bnel, "bnel") X(blez, "blez") X(blezl, "blezl")                 \
    X(bgtz, "bgtz") X(bgtzl, "bgtzl") X(bltz, "bltz") X(bltzl, "bltzl") X(bgez, "bgez") X(bgezl, "bgezl")         \
    X(bc1t, "bc1t") X(bc1f, "bc1f") X(bc1tl, "bc1tl") X(bc1fl, "bc1fl") X(mfc0, "mfc0") X(mtc0, "mtc0")

enum {
    OP_other,
#define X(id, s) OP_##id,
    OPS(X)
#undef X
    OP_count
};
static const char *const op_names[] = {"?",
#define X(id, s) s,
                                       OPS(X)
#undef X
};

static uint8_t op_flags[OP_count];
enum { F_MEM = 1, F_GENERIC = 2, F_DINLINE = 4, F_DOUBLE_OP = 8 };
static uint8_t opmap[RABBITIZER_INSTR_ID_ALL_MAX + 1];

enum { N_BUDGET, N_TICK, N_GENERIC, N_EXTERNAL, N_IDLE, N_LOOKUP, N_BADJUMP, N_UNTRANSLATED, N_UNSUPPORTED, N_BIAS,
       N_FIXED };
static const char *const fixed_names[N_FIXED] = {"_t_budget", "_t_tick", "_t_generic", "_t_external", "_t_idle",
                                                 "_t_lookup", "_t_badjump", "_t_untranslated", "_t_unsupported",
                                                 "_t_rdram_bias"};

struct xl_meta xl_m;

static XlOut *O;
static jmp_buf *fail_jb;
static char fail_msg[200];

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(fail_msg, sizeof fail_msg, fmt, ap);
    va_end(ap);
    if (!fail_jb)
        xl_die(fail_msg);
    longjmp(*fail_jb, 1);
}

static void die(const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    xl_die(buf);
}

static void *grow(void *p, uint32_t *cap, uint32_t need, size_t elem) {
    if (need <= *cap)
        return p;
    uint32_t c = *cap ? *cap : 64;
    while (c < need)
        c *= 2;
    p = xl_realloc(p, (size_t)c * elem);
    if (!p)
        die("out of memory (%u x %u)", c, (unsigned)elem);
    *cap = c;
    return p;
}

static int intern(const char *s) {
    for (uint32_t i = 0; i < M.nnames; i++)
        if (!strcmp(M.names[i], s))
            return i;
    M.names = xl_realloc(M.names, (M.nnames + 1) * sizeof *M.names);
    M.name_und = xl_realloc(M.name_und, (M.nnames + 1) * sizeof *M.name_und);
    size_t n = strlen(s) + 1;
    M.names[M.nnames] = memcpy(xl_alloc(n), s, n);
    M.name_und[M.nnames] = -1;
    return M.nnames++;
}

static int und_add(const char *s) {
    O->und = xl_realloc(O->und, (O->nund + 1) * sizeof *O->und);
    size_t n = strlen(s) + 1;
    O->und[O->nund] = memcpy(xl_alloc(n), s, n);
    return O->nund++;
}

static int und_name(int id) {
    if (M.name_und[id] < 0)
        M.name_und[id] = und_add(M.names[id]);
    return M.name_und[id];
}

static int und_vram(uint32_t v) {
    for (uint32_t i = 0; i < M.nundv; i++)
        if (M.undv[i] == v)
            return M.undv_id[i];
    char buf[16];
    snprintf(buf, sizeof buf, "_t_%08X", (unsigned)v);
    M.undv = xl_realloc(M.undv, (M.nundv + 1) * sizeof *M.undv);
    M.undv_id = xl_realloc(M.undv_id, (M.nundv + 1) * sizeof *M.undv_id);
    M.undv[M.nundv] = v;
    return M.undv_id[M.nundv++] = und_add(buf);
}

int find_u32(const uint32_t *a, uint32_t n, uint32_t v) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (a[mid] < v)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && a[lo] == v ? (int)lo : -1;
}

static int is_func(uint32_t v) { return find_u32(M.start, M.nfunc, v) >= 0; }

uint32_t word(uint32_t vram) {
    for (uint32_t i = 0; i < M.nseg; i++)
        if (M.seg[i].data && M.seg[i].vram <= vram && vram - M.seg[i].vram < M.seg[i].size &&
            M.seg[i].size - (vram - M.seg[i].vram) >= 4)
            return be32(M.seg[i].data + (vram - M.seg[i].vram));
    die("%08x not in any section", (unsigned)vram);
    return 0;
}

static int is_ovl(const Seg *s) { return CS_OV_IS_KEY(s->vram); }
static uint32_t ov_real(uint32_t v) { return CS_OV_IS_KEY(v) ? 0x80000000u | (v & CS_OV_MASK) : v; }
static int ov_holds(const Seg *s, uint32_t v) { return ov_real(s->vram) <= v && v - ov_real(s->vram) < s->size; }
static uint32_t ov_key(const Seg *s, uint32_t v) { return (s->vram & ~CS_OV_MASK) | (v & CS_OV_MASK); }

// A call target as code at `from` sees it: its own overlay's key, else the one overlay holding it. *many is set
// when several overlays hold it, so only the run time knows which is loaded.
uint32_t ov_resolve(uint32_t from, uint32_t v, int *many) {
    *many = 0;
    if (!M.novl) return v;
    const Seg *own = NULL, *hold = NULL;
    uint32_t n = 0;
    for (uint32_t i = 0; i < M.nseg; i++) {
        const Seg *s = &M.seg[i];
        if (!is_ovl(s)) continue;
        if (s->vram <= from && from - s->vram < s->size) own = s;
        if (ov_holds(s, v)) hold = s, n++;
    }
    if (own && ov_holds(own, v)) return ov_key(own, v);
    if (CS_OV_IS_KEY(v)) return 1;
    if (n > 1) *many = 1;
    return n == 1 ? ov_key(hold, v) : v;
}

// XLM3: 1 if the function at v outside this meta takes every register, 0 if not, -1 if no function starts there
static int ext_raw(uint32_t v) {
    int k = M.lazy ? find_u32(M.ext, M.next, v) : -1;
    return k < 0 ? -1 : M.ext_raw[k];
}

// Lazy: the target lies in an overlay other than the caller's, so only the run time knows its code
static int far_overlay(uint32_t target, int many) {
    return M.lazy && (many || (CS_OV_IS_KEY(target) && target != 1 && !is_func(target)));
}

// Whether any overlay function at the shared or far vram v takes every register
static int far_raw(uint32_t v) {
    for (uint32_t i = 0; i < M.nseg; i++) {
        const Seg *s = &M.seg[i];
        if (!is_ovl(s) || !ov_holds(s, v)) continue;
        uint32_t key = ov_key(s, v);
        int k = find_u32(M.start, M.nfunc, key);
        if ((k >= 0 && M.raw[k]) || ext_raw(key) > 0) return 1;
    }
    return 0;
}

static const char *rd_str(const uint8_t **p) {
    uint32_t n = rd32(*p);
    const char *s = (const char *)*p + 4;
    *p += 4 + n + (4 - n % 4);
    return s;
}

static void free_meta(void) {
    for (uint32_t i = 0; i < M.nnames; i++)
        xl_free(M.names[i]);
    void *v[] = {M.start, M.end, M.tab, M.hook_addr, M.hook_str, M.hook_name, M.entry_addr, M.entry_name,
                 M.names, M.name_und, M.undv, M.undv_id, M.raw, M.keep, M.ext};
    for (size_t i = 0; i < sizeof v / sizeof *v; i++)
        xl_free(v[i]);
    memset(&M, 0, sizeof M);
}

void load_meta(const uint8_t *b, size_t len) {
    const uint8_t *p = b;
    free_meta();
    if (len < 8 || (memcmp(p, "XLM2", 4) && memcmp(p, "XLM3", 4)))
        die("not a metapack blob");
    M.lazy = p[3] == '3';
    p += 4;
    M.nseg = rd32(p), p += 4;
    if (M.nseg > SEG_MAX)
        die("too many segments");
    for (uint32_t i = 0; i < M.nseg; i++) {
        M.seg[i].vram = rd32(p);
        M.seg[i].rom = rd32(p + 4);
        M.seg[i].size = rd32(p + 8);
        M.seg[i].data = p + 12;
        p += 12 + M.seg[i].size + (-M.seg[i].size & 3);
        M.novl += is_ovl(&M.seg[i]);
    }
    M.nfunc = rd32(p), p += 4;
    M.start = xl_alloc(M.nfunc * 4);
    M.end = xl_alloc(M.nfunc * 4);
    for (uint32_t i = 0; i < M.nfunc; i++, p += 8) {
        M.start[i] = rd32(p), M.end[i] = rd32(p + 4);
        if (i && M.start[i] <= M.start[i - 1])
            die("functions not sorted/unique at %08x", (unsigned)M.start[i]);
    }
    M.ntab = rd32(p), p += 4;
    M.tab = xl_alloc((M.ntab + 1) * sizeof *M.tab);
    for (uint32_t i = 0; i < M.ntab; i++) {
        M.tab[i].jr = rd32(p), M.tab[i].n = rd32(p + 4), M.tab[i].targets = p + 8;
        p += 8 + 4 * M.tab[i].n;
    }
    M.nhook = rd32(p), p += 4;
    M.hook_addr = xl_alloc((M.nhook + 1) * 4);
    M.hook_str = xl_alloc((M.nhook + 1) * sizeof *M.hook_str);
    M.hook_name = xl_alloc((M.nhook + 1) * sizeof *M.hook_name);
    for (uint32_t i = 0; i < M.nhook; i++) {
        M.hook_addr[i] = rd32(p), p += 4;
        M.hook_str[i] = rd_str(&p);
    }
    M.nentry = rd32(p), p += 4;
    M.entry_addr = xl_alloc((M.nentry + 1) * 4);
    M.entry_name = xl_alloc((M.nentry + 1) * sizeof *M.entry_name);
    for (uint32_t i = 0; i < M.nentry; i++) {
        M.entry_addr[i] = rd32(p), p += 4;
        M.entry_name[i] = rd_str(&p);
    }
    if (M.lazy) {  // the overlays this meta leaves out (ranges without bytes), then the functions it leaves out
        uint32_t nr = rd32(p);
        p += 4;
        if (M.nseg + nr > SEG_MAX)
            die("too many segments");
        for (uint32_t i = 0; i < nr; i++, p += 12) {
            Seg *s = &M.seg[M.nseg++];
            s->vram = rd32(p), s->rom = rd32(p + 4), s->size = rd32(p + 8), s->data = NULL;
            M.novl += is_ovl(s);
        }
        M.next = rd32(p), p += 4;
        M.ext = xl_alloc(M.next * 4 + 4);
        for (uint32_t i = 0; i < M.next; i++) M.ext[i] = rd32(p + 4 * i);
        M.ext_raw = p + 4 * M.next;
        p += 4 * M.next + ((M.next + 3) & ~3u);
        for (uint32_t i = 1; i < M.next; i++)
            if (M.ext[i] <= M.ext[i - 1])
                die("extern functions not sorted/unique at %08x", (unsigned)M.ext[i]);
    }
    if ((size_t)(p - b) != len)
        die("metapack blob: %u bytes parsed of %u", (unsigned)(p - b), (unsigned)len);
    for (int i = 0; i < N_FIXED; i++)
        intern(fixed_names[i]);
    for (uint32_t i = 0; i < M.nhook; i++) {
        char buf[96];
        snprintf(buf, sizeof buf, "_h_%s", M.hook_str[i]);
        M.hook_name[i] = intern(buf);
    }
}

void init_ops(void) {
    RabbitizerConfig_Cfg.pseudos.enablePseudos = false;
    for (int id = 0; id < RABBITIZER_INSTR_ID_ALL_MAX; id++) {
        const char *n = RabbitizerInstrId_getOpcodeName(id);
        opmap[id] = OP_other;
        for (int k = 1; k < OP_count && n; k++)
            if (!strcmp(n, op_names[k])) {
                opmap[id] = k;
                break;
            }
    }
    static const uint8_t mem[] = {OP_lw, OP_sw, OP_lh, OP_lhu, OP_lb, OP_lbu, OP_sh, OP_sb, OP_lwc1, OP_swc1,
                                  OP_ldc1, OP_sdc1};
    static const uint8_t gen[] = {OP_divu, OP_brk, OP_cache, OP_lwl, OP_lwr,
                                  OP_swl, OP_swr, OP_ld, OP_sd, OP_cvt_w_s, OP_cvt_d_s, OP_cfc1, OP_ctc1,
                                  OP_mfc0, OP_mtc0};
    static const uint8_t dop[] = {OP_add_d, OP_sub_d, OP_mul_d, OP_div_d};
    static const uint8_t dinl[] = {OP_cvt_d_s, OP_cvt_s_d, OP_cvt_d_w, OP_trunc_w_d, OP_c_lt_d, OP_c_le_d,
                                   OP_c_eq_d, OP_mov_d, OP_neg_d};
    for (size_t i = 0; i < sizeof mem; i++) op_flags[mem[i]] |= F_MEM;
    for (size_t i = 0; i < sizeof gen; i++) op_flags[gen[i]] |= F_GENERIC;
    for (size_t i = 0; i < sizeof dop; i++) op_flags[dop[i]] |= F_DOUBLE_OP | F_DINLINE;
    for (size_t i = 0; i < sizeof dinl; i++) op_flags[dinl[i]] |= F_DINLINE;
}

typedef struct {
    RabbitizerInstruction ri;
    uint32_t raw, vram;
    uint8_t op;
} Insn;

#define D(x) ((x)->ri.descriptor)
static int isBranch(const Insn *i) { return RabbitizerInstrDescriptor_isBranch(D(i)); }
static int isJump(const Insn *i) { return RabbitizerInstrDescriptor_isJump(D(i)); }
static uint32_t branch_vram(const Insn *i) { return RabbitizerInstruction_getBranchVramGeneric(&i->ri); }
// j/jal take their region from the real pc: overlay keys from slot 16 on (0x90000000+) aren't in the 0x8 region
uint32_t j_target(uint32_t vram, uint32_t raw) {
    return ((ov_real(vram) + 4) & 0xF0000000u) | (raw & 0x3FFFFFFu) << 2;
}
static uint32_t index_vram(const Insn *i) { return j_target(i->vram, i->raw); }
static uint32_t jump_key(uint32_t func, const Insn *i) {
    int many;
    return ov_resolve(func, index_vram(i), &many);
}

enum { IT_I, IT_LBL, IT_FLBL, IT_LIT, IT_BR, IT_BARRIER, IT_TABLE };
enum { V_CONST, V_NAME, V_FUNC, V_LABEL };

typedef struct {
    uint8_t kind, n, x, tag;
    uint32_t a;
} Item;

typedef struct {
    Item *it;
    uint32_t n, cap;
    uint16_t *ops;
    uint32_t nops, capops;
    int32_t *lpos;
    uint32_t nlab, caplab;
} Em;

static Em E;

static Item *push_item(uint8_t kind) {
    E.it = grow(E.it, &E.cap, E.n + 1, sizeof *E.it);
    Item *it = &E.it[E.n++];
    *it = (Item){.kind = kind};
    return it;
}

static void ig(const uint16_t *ops, int n) {
    Item *it = push_item(IT_I);
    it->n = n;
    for (int i = 0; i < n; i++)
        it->tag |= sh_clobbers_t(ops[i]);
    if (n <= 2) {
        it->a = ops[0] | (n == 2 ? (uint32_t)ops[1] << 16 : 0);
    } else {
        E.ops = grow(E.ops, &E.capops, E.nops + n, sizeof *E.ops);
        it->a = E.nops;
        memcpy(E.ops + E.nops, ops, n * sizeof *ops);
        E.nops += n;
    }
}

static uint16_t item_op(const Item *it, int k) {
    return it->n <= 2 ? (uint16_t)(it->a >> (16 * k)) : E.ops[it->a + k];
}

#define I1(a) ig((const uint16_t[]){a}, 1)
#define I2(a, b) ig((const uint16_t[]){a, b}, 2)
#define I3(a, b, c) ig((const uint16_t[]){a, b, c}, 3)
#define I4(a, b, c, d) ig((const uint16_t[]){a, b, c, d}, 4)
#define I5(a, b, c, d, e) ig((const uint16_t[]){a, b, c, d, e}, 5)

static uint32_t new_label(void) {
    E.lpos = grow(E.lpos, &E.caplab, E.nlab + 1, sizeof *E.lpos);
    E.lpos[E.nlab] = -1;
    return E.nlab++;
}

static void em_label(uint32_t id) { push_item(IT_LBL)->a = id; }
static void em_flabel(void) { push_item(IT_FLBL); }
static void em_barrier(void) { push_item(IT_BARRIER); }
static void em_br(char cond, uint32_t id) {
    Item *it = push_item(IT_BR);
    it->x = cond, it->a = id;
}
static void em_lit(int rd, int tag, uint32_t v) {
    Item *it = push_item(IT_LIT);
    it->x = rd, it->tag = tag, it->a = v;
}

static void em_reset(void) {
    E.n = E.nops = E.nlab = 0;
}

typedef struct FT FT;

typedef struct {
    int kind;
    const int8_t *slots;
    int nslots;
    int8_t map[64], fixed[64], shadow[64];
    uint64_t in_map, has_fixed, has_shadow;
    uint64_t dirty, pinned, fixed_written, fixed_read, now_written;
    int8_t lru[64];
    int nlru;
} Cache;

static const int8_t GPR_SLOTS[] = {3, 4, 5, 6, 7, 8, 9, 10};
static const int8_t FPR_SLOTS[] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

static void cache_init(Cache *c, int kind) {
    c->kind = kind;
    c->slots = kind ? FPR_SLOTS : GPR_SLOTS;
    c->nslots = kind ? 12 : 8;
}

static void c_load(Cache *c, int k, int sh) {
    if (!c->kind) {
        if (k == 0)
            I1(MOVI(0, sh));
        else
            I1(k < 16 ? MOVL_LDD(4 * k, 14, sh) : MOVL_LDD(4 * (k - 16), 12, sh));
    } else {
        if (4 * k > 127)
            fail("fpr slot %d out of range", k);
        I1(MOVI(4 * k, 0));
        I1(FMOVS_LD0(11, sh));
    }
}

static void c_store(Cache *c, int k, int sh) {
    if (!c->kind) {
        I1(k < 16 ? MOVL_STD(sh, 4 * k, 14) : MOVL_STD(sh, 4 * (k - 16), 12));
    } else {
        if (4 * k > 127)
            fail("fpr slot %d out of range", k);
        I1(MOVI(4 * k, 0));
        I1(FMOVS_ST0(sh, 11));
    }
}

static void c_touch(Cache *c, int k) {
    for (int i = 0; i < c->nlru; i++)
        if (c->lru[i] == k) {
            memmove(c->lru + i, c->lru + i + 1, c->nlru - i - 1);
            c->nlru--;
            break;
        }
    c->lru[c->nlru++] = k;
}

static int c_alloc(Cache *c, int k) {
    uint32_t used = 0;
    int r;
    FOR_BITS(r, c->in_map) used |= 1u << c->map[r];
    FOR_BITS(r, c->has_fixed) used |= 1u << c->fixed[r];
    FOR_BITS(r, c->has_shadow) used |= 1u << c->shadow[r];
    int sh = -1;
    for (int i = 0; i < c->nslots; i++)
        if (!(used & (1u << c->slots[i]))) {
            sh = c->slots[i];
            break;
        }
    if (sh < 0) {
        int vi = -1;
        for (int i = 0; i < c->nlru; i++)
            if (!(c->pinned & BIT(c->lru[i]))) {
                vi = i;
                break;
            }
        if (vi < 0)
            die("register cache: every slot pinned");
        int victim = c->lru[vi];
        sh = c->map[victim];
        c->in_map &= ~BIT(victim);
        memmove(c->lru + vi, c->lru + vi + 1, c->nlru - vi - 1);
        c->nlru--;
        if (c->dirty & BIT(victim)) {
            c->dirty &= ~BIT(victim);
            c_store(c, victim, sh);
        }
    }
    c->map[k] = sh;
    c->in_map |= BIT(k);
    return sh;
}

static int c_read(Cache *c, int k) {
    if (c->has_fixed & BIT(k))
        return c->fixed[k];
    if (!(c->in_map & BIT(k)))
        c_load(c, k, c_alloc(c, k));
    c_touch(c, k);
    c->pinned |= BIT(k);
    return c->map[k];
}

static int c_write(Cache *c, int k) {
    if (!c->kind && k == 0)
        return 2;
    c->now_written |= BIT(k);
    if (c->has_fixed & BIT(k))
        return c->fixed[k];
    if (!(c->in_map & BIT(k)))
        c_alloc(c, k);
    c_touch(c, k);
    c->dirty |= BIT(k);
    c->pinned |= BIT(k);
    return c->map[k];
}

static void c_flush(Cache *c, uint64_t only) {
    int k;
    FOR_BITS(k, c->dirty & only) c_store(c, k, c->map[k]);
    c->dirty = 0;
}

static void c_invalidate(Cache *c) {
    c->in_map = 0;
    c->nlru = 0;
    c->dirty = 0;
}

static void c_post_op(Cache *c) {
    int k;
    FOR_BITS(k, c->now_written & c->has_shadow) {
        int src = (c->has_fixed & BIT(k)) ? c->fixed[k] : c->map[k];
        I1(MOV(src, c->shadow[k]));
        I1(ADD(13, c->shadow[k]));
    }
    c->now_written = 0;
}

static void c_store_fixed(Cache *c, uint64_t only) {
    int k;
    FOR_BITS(k, c->fixed_written & c->has_fixed & only) c_store(c, k, c->fixed[k]);
}

static void c_load_fixed(Cache *c, uint64_t only) {
    int k;
    FOR_BITS(k, c->has_fixed & c->fixed_read & only) c_load(c, k, c->fixed[k]);
    FOR_BITS(k, c->has_shadow) {
        if (c->has_fixed & BIT(k))
            I1(MOV(c->fixed[k], c->shadow[k]));
        else
            c_load(c, k, c->shadow[k]);
        I1(ADD(13, c->shadow[k]));
    }
}

struct FT {
    int level;
    uint32_t func, lo, hi, nwords;
    Insn *ins;
    uint32_t n;
    Cache g, f;
    uint32_t ntabs, *tabs, *tab_label;
    uint8_t *leader, *loophead, *reach;  // reach: some path from the entry or a jump table gets there
    int mmio;
    int raw, keep;
    uint32_t generic;
};

static uint32_t lab(FT *t, uint32_t v) {
    if (v < t->lo || v > t->hi || (v - t->lo) & 3)
        fail("%08x: label %08x outside the function", (unsigned)t->func, (unsigned)v);
    return (v - t->lo) / 4;
}

static int ft_table(FT *t, uint32_t jr) {
    for (uint32_t i = 0; i < t->ntabs; i++)
        if (M.tab[t->tabs[i]].jr == jr)
            return i;
    return -1;
}

static void sync_(FT *t) {
    c_flush(&t->g, ALL);
    c_flush(&t->f, ALL);
}

static void drop(FT *t) {
    c_invalidate(&t->g);
    c_invalidate(&t->f);
}

static void const_(int rd, uint32_t value) {
    int32_t sv = (int32_t)value;
    if (sv >= -128 && sv <= 127)
        I1(MOVI(sv, rd));
    else
        em_lit(rd, V_CONST, value);
}

static void addr_r0(int base, int32_t imm) {
    I1(MOV(base, 0));
    if (imm && imm >= -128 && imm <= 127)
        I1(ADDI(imm, 0));
    else if (imm) {
        const_(1, (uint32_t)imm);
        I1(ADD(1, 0));
    }
}

static void two_op(uint16_t base, int d, int a, int b, int comm, int fp) {
    uint16_t mov = fp ? 0xF00C : 0x6003;
    int tmp = 1;
    if (d == a) {
        I1(NM(base, b, d));
    } else if (d == b && comm) {
        I1(NM(base, a, d));
    } else if (d == b) {
        I1(NM(mov, b, tmp));
        I1(NM(mov, a, d));
        I1(NM(base, tmp, d));
    } else {
        I1(NM(mov, a, d));
        I1(NM(base, b, d));
    }
}

// Every guest register back in tctx: before code that may read any of them
static void store_all(FT *t) {
    sync_(t);
    c_store_fixed(&t->g, ALL);
    c_store_fixed(&t->f, ALL);
}

static void before_call(FT *t, int abi) {
    if (abi && t->level >= 2) {
        c_flush(&t->g, G_STORE);
        c_flush(&t->f, F_STORE);
    } else {
        sync_(t);
    }
    c_store_fixed(&t->g, abi ? G_STORE : ALL);
    c_store_fixed(&t->f, abi ? F_STORE : ALL);
}

static void after_call(FT *t, int abi) {
    drop(t);
    c_load_fixed(&t->g, abi ? G_RELOAD : ALL);
    c_load_fixed(&t->f, abi ? F_RELOAD : ALL);
}

static void leave_function(FT *t) {
    if (t->raw) {
        store_all(t);
        return;
    }
    if (t->level >= 2) {
        c_flush(&t->g, G_EXIT);
        c_flush(&t->f, F_EXIT);
    } else {
        sync_(t);
    }
    c_store_fixed(&t->g, G_EXIT);
    c_store_fixed(&t->f, F_EXIT);
}

static void call_c(FT *t, int name, int nargs, uint32_t a0, uint32_t a1) {
    before_call(t, 0);
    drop(t);
    I1(MOV(14, 4));
    I1(ADDI(-12, 4));
    if (nargs > 0) const_(5, a0);
    if (nargs > 1) const_(6, a1);
    em_lit(1, V_NAME, name);
    I2(JSR(1), NOP);
    after_call(t, 0);
}

static void backedge_poll(FT *t) {
    uint32_t skip = new_label();
    em_lit(1, V_NAME, N_BUDGET);
    I3(MOVL_LDI(1, 2), DT(2), MOVL_STI(2, 1));
    em_br('f', skip);
    call_c(t, N_TICK, 0, 0, 0);
    em_label(skip);
}

static void get_half(FT *t, int k, int dst) {
    Cache *f = &t->f;
    int src = (f->has_fixed & BIT(k)) ? f->fixed[k] : (f->in_map & BIT(k)) ? f->map[k] : -1;
    if (src >= 0)
        I1(FMOV(src, dst));
    else
        I2(MOVI(4 * k, 0), FMOVS_LD0(11, dst));
}

static void put_half(FT *t, int k, int src) {
    Cache *f = &t->f;
    if (f->has_fixed & BIT(k)) {
        I1(FMOV(src, f->fixed[k]));
        f->now_written |= BIT(k);
    } else if (f->in_map & BIT(k)) {
        I1(FMOV(src, f->map[k]));
        f->dirty |= BIT(k);
    } else {
        I2(MOVI(4 * k, 0), FMOVS_ST0(src, 11));
    }
}

#define TO_DOUBLE() I5(STS_FPSCR(1), MOVI(8, 2), SHLL16(2), XOR(2, 1), LDS_FPSCR(1))
#define TO_SINGLE() I2(XOR(2, 1), LDS_FPSCR(1))

static void double_op(FT *t, int op, int fs, int ft, int fd) {
    if (op == OP_cvt_d_s || op == OP_cvt_d_w) {
        get_half(t, fs, 2);
        I1(FLDS(2));
        TO_DOUBLE();
        I1(op == OP_cvt_d_s ? FCNVSD(0) : FLOAT(0));
        TO_SINGLE();
    } else {
        int cmp = op == OP_c_lt_d || op == OP_c_le_d || op == OP_c_eq_d;
        get_half(t, fs + 1, 0);
        get_half(t, fs, 1);
        if ((op_flags[op] & F_DOUBLE_OP) || cmp) {
            get_half(t, ft + 1, 2);
            get_half(t, ft, 3);
        }
        TO_DOUBLE();
        switch (op) {
        case OP_add_d: I1(FADD(2, 0)); break;
        case OP_sub_d: I1(FSUB(2, 0)); break;
        case OP_mul_d: I1(FMUL(2, 0)); break;
        case OP_div_d: I1(FDIV(2, 0)); break;
        case OP_neg_d: I1(FNEG(0)); break;
        case OP_cvt_s_d: I1(FCNVDS(0)); break;
        case OP_trunc_w_d: I1(FTRC(0)); break;
        case OP_c_lt_d: I1(FCMPGT(0, 2)); break;
        case OP_c_le_d: I1(FCMPGT(2, 0)); break;
        case OP_c_eq_d: I1(FCMPEQ(0, 2)); break;
        }
        TO_SINGLE();
        if (cmp) {
            I1(MOVT(2));
            if (op == OP_c_le_d)
                I3(MOVI(1, 1), SUB(2, 1), MOV(1, 2));
            I2(MOVI(-12, 0), MOVL_ST0(2, 14));
            return;
        }
        if (op == OP_cvt_s_d || op == OP_trunc_w_d) {
            I1(FSTS(2));
            put_half(t, fd, 2);
            return;
        }
    }
    put_half(t, fd + 1, 0);
    put_half(t, fd, 1);
}

static int shadow_mem(FT *t, int op, int rs, int rt, int ft, int32_t imm) {
    Cache *g = &t->g, *f = &t->f;
    if (!(g->has_shadow & BIT(rs)))
        return 0;
    int S = g->shadow[rs];
    if (op == OP_lw || op == OP_sw) {
        if (imm < -128 || imm > 127)
            return 0;
        int small = imm >= 0 && imm <= 60 && imm % 4 == 0;
        if (op == OP_lw) {
            int d = c_write(g, rt);
            if (small) I1(MOVL_LDD(imm, S, d));
            else I2(MOVI(imm, 0), MOVL_LD0(S, d));
        } else {
            int s = c_read(g, rt);
            if (small) I1(MOVL_STD(s, imm, S));
            else I2(MOVI(imm, 0), MOVL_ST0(s, S));
        }
        return 1;
    }
    if (op == OP_lwc1 || op == OP_swc1 || op == OP_ldc1 || op == OP_sdc1) {
        if (imm < -128 || imm > 123)
            return 0;
        if (op == OP_lwc1) {
            int d = c_write(f, ft);
            I2(MOVI(imm, 0), FMOVS_LD0(S, d));
        } else if (op == OP_swc1) {
            int s = c_read(f, ft);
            I2(MOVI(imm, 0), FMOVS_ST0(s, S));
        } else if (op == OP_ldc1) {
            int lo = c_write(f, ft), hi = c_write(f, ft + 1);
            I4(MOVI(imm, 0), FMOVS_LD0(S, hi), ADDI(4, 0), FMOVS_LD0(S, lo));
        } else {
            int lo = c_read(f, ft), hi = c_read(f, ft + 1);
            I4(MOVI(imm, 0), FMOVS_ST0(hi, S), ADDI(4, 0), FMOVS_ST0(lo, S));
        }
        return 1;
    }
    int hb = op == OP_lh || op == OP_lhu || op == OP_lb || op == OP_lbu || op == OP_sh || op == OP_sb;
    if (rs != 29 || !hb)
        return 0;
    int half = op == OP_lh || op == OP_lhu || op == OP_sh;
    int32_t off = imm ^ (half ? 2 : 3);
    if (off < -128 || off > 127)
        return 0;
    if (op == OP_lh || op == OP_lhu || op == OP_lb || op == OP_lbu) {
        int d = c_write(g, rt);
        I2(MOVI(off, 0), half ? MOVW_LD0(S, d) : MOVB_LD0(S, d));
        if (op == OP_lhu || op == OP_lbu)
            I1(half ? EXTUW(d, d) : EXTUB(d, d));
    } else {
        int s = c_read(g, rt);
        I2(MOVI(off, 0), half ? MOVW_ST0(s, S) : MOVB_ST0(s, S));
    }
    return 1;
}

static void shift_steps(int sa, int *steps, int *n) {
    static const int sz[] = {16, 8, 2, 1};
    *n = 0;
    for (int i = 0; i < 4; i++)
        while (sa >= sz[i]) {
            if (*n < 32) steps[*n] = sz[i];
            (*n)++;
            sa -= sz[i];
        }
}

// The SH-4 two-operand forms each MIPS op becomes: and, xor, or, add, sub; fadd, fsub, fmul, fdiv
static const uint16_t two_op_base[OP_count] = {
    [OP_and] = 0x2009, [OP_andi] = 0x2009, [OP_xor] = 0x200A, [OP_xori] = 0x200A, [OP_or] = 0x200B, [OP_ori] = 0x200B,
    [OP_nor] = 0x200B, [OP_addu] = 0x300C, [OP_add] = 0x300C, [OP_subu] = 0x3008, [OP_sub] = 0x3008,
    [OP_add_s] = 0xF000, [OP_sub_s] = 0xF001, [OP_mul_s] = 0xF002, [OP_div_s] = 0xF003,
};

static uint16_t shift_by(int left, int st, int d) {
    switch (st) {
    case 1: return left ? SHLL(d) : SHLR(d);
    case 2: return left ? SHLL2(d) : SHLR2(d);
    case 8: return left ? SHLL8(d) : SHLR8(d);
    default: return left ? SHLL16(d) : SHLR16(d);
    }
}

// Each op_* emits its class's ops and returns 1, or returns 0 for an op outside it
static int op_alu(Cache *g, int op, int rs, int rt, int rd, int32_t imm) {
    int a, b, d;
    switch (op) {
    case OP_addu: case OP_add: case OP_or: case OP_and: case OP_xor: case OP_nor: case OP_subu: case OP_sub:
    case OP_slt: case OP_sltu:
        if ((op == OP_addu || op == OP_add || op == OP_or) && rt == 0) {
            a = c_read(g, rs);
            d = c_write(g, rd);
            if (d != a) I1(MOV(a, d));
            return 1;
        }
        if ((op == OP_addu || op == OP_or) && rs == 0) {
            a = c_read(g, rt);
            d = c_write(g, rd);
            if (d != a) I1(MOV(a, d));
            return 1;
        }
        a = c_read(g, rs);
        b = c_read(g, rt);
        d = c_write(g, rd);
        if (op == OP_slt || op == OP_sltu) {
            I1(op == OP_slt ? CMPGT(a, b) : CMPHI(a, b));
            I1(MOVT(d));
        } else {
            two_op(two_op_base[op], d, a, b, op != OP_subu && op != OP_sub, 0);
            if (op == OP_nor) I1(NOT(d, d));
        }
        break;
    case OP_addiu: case OP_addi:
        if (rs == 0) {
            d = c_write(g, rt);
            const_(d, (uint32_t)imm);
            return 1;
        }
        a = c_read(g, rs);
        d = c_write(g, rt);
        if (d != a) I1(MOV(a, d));
        if (imm >= -128 && imm <= 127) {
            if (imm) I1(ADDI(imm, d));
        } else {
            const_(1, (uint32_t)imm);
            I1(ADD(1, d));
        }
        break;
    case OP_andi: case OP_ori: case OP_xori:
        if (rs == 0) {
            d = c_write(g, rt);
            const_(d, op != OP_andi ? (uint32_t)imm : 0);
            return 1;
        }
        a = c_read(g, rs);
        d = c_write(g, rt);
        const_(1, (uint32_t)imm);
        two_op(two_op_base[op], d, a, 1, 1, 0);
        break;
    case OP_lui:
        d = c_write(g, rt);
        const_(d, ((uint32_t)imm & 0xFFFF) << 16);
        break;
    case OP_slti: case OP_sltiu:
        a = c_read(g, rs);
        d = c_write(g, rt);
        const_(1, (uint32_t)imm);
        I1(op == OP_slti ? CMPGT(a, 1) : CMPHI(a, 1));
        I1(MOVT(d));
        break;
    default: return 0;
    }
    return 1;
}

static int op_shift(Cache *g, int op, int rs, int rt, int rd, int sa) {
    int a, b, d;
    switch (op) {
    case OP_sll: case OP_srl: case OP_sra: {
        a = c_read(g, rt);
        d = c_write(g, rd);
        if (d != a) I1(MOV(a, d));
        if (sa == 0) return 1;
        if (op == OP_sll || op == OP_srl) {
            int steps[32], n;
            shift_steps(sa, steps, &n);
            if (n <= 2) {
                for (int i = 0; i < n; i++) I1(shift_by(op == OP_sll, steps[i], d));
            } else {
                I1(MOVI(op == OP_sll ? sa : -sa, 1));
                I1(SHLD(1, d));
            }
        } else if (sa <= 2) {
            for (int i = 0; i < sa; i++) I1(SHAR(d));
        } else {
            I1(MOVI(-sa, 1));
            I1(SHAD(1, d));
        }
        break;
    }
    case OP_sllv: case OP_srlv: case OP_srav:  // shld/shad: a negative count shifts right by its magnitude
        b = c_read(g, rs);
        a = c_read(g, rt);
        d = c_write(g, rd);
        I1(MOVI(31, 1));
        I1(AND(b, 1));
        if (op != OP_sllv) I1(NEG(1, 1));
        if (d != a) I1(MOV(a, d));
        I1(op == OP_srav ? SHAD(1, d) : SHLD(1, d));
        break;
    default: return 0;
    }
    return 1;
}

static int op_muldiv(Cache *g, int op, int rs, int rt, int rd) {
    int a, b, d;
    switch (op) {
    case OP_mult: case OP_multu:
        a = c_read(g, rs);
        b = c_read(g, rt);
        I1(op == OP_mult ? DMULS(a, b) : DMULU(a, b));
        I1(STS_MACL(1));
        I1(MOVI(-4, 0));
        I1(MOVL_ST0(1, 14));
        I1(STS_MACH(1));
        I1(MOVI(-8, 0));
        I1(MOVL_ST0(1, 14));
        break;
    case OP_div:  // exact in double: its rounding error is below any quotient's distance to the next integer
        a = c_read(g, rs);
        b = c_read(g, rt);
        TO_DOUBLE();
        I4(LDS_FPUL(b), FLOAT(2), LDS_FPUL(a), FLOAT(0));
        I2(FDIV(2, 0), FTRC(0));
        TO_SINGLE();
        I3(STS_FPUL(1), MOVI(-4, 0), MOVL_ST0(1, 14));
        I4(MULL(1, b), STS_MACL(2), MOV(a, 1), SUB(2, 1));
        I2(MOVI(-8, 0), MOVL_ST0(1, 14));
        break;
    case OP_mflo: case OP_mfhi:
        d = c_write(g, rd);
        I1(MOVI(op == OP_mflo ? -4 : -8, 0));
        I1(MOVL_LD0(14, d));
        break;
    case OP_mtlo: case OP_mthi:
        a = c_read(g, rs);
        I1(MOVI(op == OP_mtlo ? -4 : -8, 0));
        I1(MOVL_ST0(a, 14));
        break;
    default: return 0;
    }
    return 1;
}

static int op_mem(Cache *g, Cache *f, int op, int rs, int rt, int32_t imm) {
    int ft = rt, b, d, s;
    switch (op) {
    case OP_lw: case OP_lh: case OP_lhu: case OP_lb: case OP_lbu: {
        int half = op == OP_lh || op == OP_lhu;
        b = c_read(g, rs);
        d = c_write(g, rt);
        addr_r0(b, imm);
        if (op == OP_lw) {
            I1(MOVL_LD0(13, d));
        } else {
            I1(XORI0(half ? 2 : 3));
            I1(half ? MOVW_LD0(13, d) : MOVB_LD0(13, d));
            if (op == OP_lhu || op == OP_lbu) I1(half ? EXTUW(d, d) : EXTUB(d, d));
        }
        break;
    }
    case OP_sw: case OP_sh: case OP_sb:
        b = c_read(g, rs);
        s = c_read(g, rt);
        addr_r0(b, imm);
        if (op != OP_sw) I1(XORI0(op == OP_sh ? 2 : 3));
        I1(op == OP_sw ? MOVL_ST0(s, 13) : op == OP_sh ? MOVW_ST0(s, 13) : MOVB_ST0(s, 13));
        break;
    case OP_lwc1:
        b = c_read(g, rs);
        d = c_write(f, ft);
        addr_r0(b, imm);
        I1(FMOVS_LD0(13, d));
        break;
    case OP_swc1:
        b = c_read(g, rs);
        s = c_read(f, ft);
        addr_r0(b, imm);
        I1(FMOVS_ST0(s, 13));
        break;
    case OP_ldc1: {
        b = c_read(g, rs);
        int lo = c_write(f, ft), hi = c_write(f, ft + 1);
        addr_r0(b, imm);
        I1(FMOVS_LD0(13, hi));
        I1(ADDI(4, 0));
        I1(FMOVS_LD0(13, lo));
        break;
    }
    case OP_sdc1: {
        b = c_read(g, rs);
        int lo = c_read(f, ft), hi = c_read(f, ft + 1);
        addr_r0(b, imm);
        I1(FMOVS_ST0(hi, 13));
        I1(ADDI(4, 0));
        I1(FMOVS_ST0(lo, 13));
        break;
    }
    default: return 0;
    }
    return 1;
}

static int op_fpu(Cache *g, Cache *f, int op, int rt, int fs, int ft, int fd) {
    int a, b, d, s;
    switch (op) {
    case OP_mtc1:
        a = c_read(g, rt);
        d = c_write(f, fs);
        I1(LDS_FPUL(a));
        I1(FSTS(d));
        break;
    case OP_mfc1:
        s = c_read(f, fs);
        d = c_write(g, rt);
        I1(FLDS(s));
        I1(STS_FPUL(d));
        break;
    case OP_add_s: case OP_sub_s: case OP_mul_s: case OP_div_s:
        a = c_read(f, fs);
        b = c_read(f, ft);
        d = c_write(f, fd);
        two_op(two_op_base[op], d, a, b, op == OP_add_s || op == OP_mul_s, 1);
        break;
    case OP_mov_s: case OP_neg_s:
        a = c_read(f, fs);
        d = c_write(f, fd);
        if (d != a) I1(FMOV(a, d));
        if (op == OP_neg_s) I1(FNEG(d));
        break;
    case OP_cvt_s_w:
        a = c_read(f, fs);
        d = c_write(f, fd);
        I1(FLDS(a));
        I1(FLOAT(d));
        break;
    case OP_trunc_w_s:
        a = c_read(f, fs);
        d = c_write(f, fd);
        I1(FTRC(a));
        I1(FSTS(d));
        break;
    case OP_sqrt_s: case OP_abs_s:
        a = c_read(f, fs);
        d = c_write(f, fd);
        if (d != a) I1(FMOV(a, d));
        I1(op == OP_sqrt_s ? FSQRT(d) : FABS(d));
        break;
    default: return 0;
    }
    return 1;
}

// FPSCR.DN reads denormals as zero; the R4300 compares them as they are (a pointer seen as a float is one).
// Flushing only changes an answer when it makes the operands equal: then compare the bits as ordered keys
static int op_fcmp(Cache *f, int op, int fs, int ft) {
    int a, b;
    switch (op) {
    case OP_c_lt_s: case OP_c_le_s: case OP_c_eq_s: {
        a = c_read(f, fs);
        b = c_read(f, ft);
        uint32_t done = new_label();
        if (op == OP_c_le_s) {
            I1(FCMPGT(b, a));
            I1(MOVT(2));
            I1(MOVI(1, 1));
            I1(SUB(2, 1));
        } else {
            I1(op == OP_c_lt_s ? FCMPGT(a, b) : FCMPEQ(a, b));
            I1(MOVT(1));
        }
        if (op != OP_c_eq_s) I1(FCMPEQ(a, b));
        em_br('f', done);
        I1(FLDS(a));
        I1(STS_FPUL(1));
        I1(FLDS(b));
        I1(STS_FPUL(2));
        for (int r = 1; r <= 2; r++) {  // sign-magnitude to two's complement: -0 and +0 meet at 0
            uint32_t pos = new_label();
            I1(CMPPZ(r));
            em_br('t', pos);
            I1(SHLL(r));
            I1(SHLR(r));
            I1(NEG(r, r));
            em_label(pos);
        }
        I1(op == OP_c_eq_s ? CMPEQ(1, 2) : op == OP_c_lt_s ? CMPGT(1, 2) : CMPGE(1, 2));
        I1(MOVT(1));
        em_label(done);
        I1(MOVI(-12, 0));
        I1(MOVL_ST0(1, 14));
        break;
    }
    default: return 0;
    }
    return 1;
}

static void op_(FT *t, const Insn *ins) {
    int op = ins->op;
    Cache *g = &t->g, *f = &t->f;
    uint32_t raw = ins->raw;
    if (raw == 0)
        return;
    if (t->mmio && (op_flags[op] & F_MEM) && ((raw >> 21) & 31) != 29) {  // the stack is never KSEG1
        t->generic++;
        call_c(t, N_GENERIC, 2, raw, ov_real(ins->vram));
        return;
    }
    int fmt = raw >> 26 == 0x11 ? (int)((raw >> 21) & 31) : -1;
    if (op_flags[op] & F_DINLINE) {
        double_op(t, op, (raw >> 11) & 31, (raw >> 16) & 31, (raw >> 6) & 31);
        return;
    }
    if ((op_flags[op] & F_GENERIC) || fmt == 0x11 || (fmt == 0x14 && op != OP_cvt_s_w)) {
        t->generic++;
        call_c(t, N_GENERIC, 2, raw, ov_real(ins->vram));
        return;
    }
    int rs = (raw >> 21) & 31, rt = (raw >> 16) & 31, rd = (raw >> 11) & 31, sa = (raw >> 6) & 31;
    int32_t imm = 0;
    if (RabbitizerInstrDescriptor_isIType(D(ins))) {
        if (!RabbitizerInstruction_hasOperandAlias(&ins->ri, RAB_OPERAND_cpu_immediate))
            die("%08x: I-type without an immediate", (unsigned)ins->vram);
        imm = RabbitizerInstruction_getProcessedImmediate(&ins->ri);
    }
    if (t->level >= 2 && (op_flags[op] & F_MEM) && shadow_mem(t, op, rs, rt, rt, imm))
        return;
    if (op_mem(g, f, op, rs, rt, imm) || op_alu(g, op, rs, rt, rd, imm) || op_shift(g, op, rs, rt, rd, sa) ||
        op_fpu(g, f, op, rt, rd, rt, sa) || op_fcmp(f, op, rd, rt) || op_muldiv(g, op, rs, rt, rd))
        return;
    fail("%08x: no template for %s (%08x)", (unsigned)ins->vram, RabbitizerInstrId_getOpcodeName(ins->ri.uniqueId),
         (unsigned)raw);
}

static void after_op(FT *t) {
    c_post_op(&t->g);
    t->g.pinned = 0;
    t->f.pinned = 0;
    if (t->level == 0) {
        sync_(t);
        drop(t);
    }
}

static int emit_slot(FT *t, const Insn *slot) {
    uint32_t start = E.n;
    op_(t, slot);
    after_op(t);
    for (uint32_t i = start; i < E.n; i++)
        if (E.it[i].kind == IT_I && (E.it[i].tag & 1))
            return 1;
    return 0;
}

enum { C_FALSE, C_TRUE, C_ALWAYS, C_NEVER };

static int cond(FT *t, const Insn *ins) {
    int op = ins->op;
    Cache *g = &t->g;
    int rs = (ins->raw >> 21) & 31, rt = (ins->raw >> 16) & 31;
    const char *name = RabbitizerInstrId_getOpcodeName(ins->ri.uniqueId);
    if (!strncmp(name, "bc1", 3)) {
        I1(MOVI(-12, 0));
        I1(MOVL_LD0(14, 1));
        I1(TST(1, 1));
        return op == OP_bc1f || op == OP_bc1fl;
    }
    if (op == OP_beq || op == OP_beql || op == OP_bne || op == OP_bnel) {
        int want = op == OP_beq || op == OP_beql;
        if (rs == rt)
            return want ? C_ALWAYS : C_NEVER;
        if (rt == 0 || rs == 0) {
            int a = c_read(g, rs ? rs : rt);
            I1(TST(a, a));
        } else {
            int a = c_read(g, rs), b = c_read(g, rt);
            I1(CMPEQ(a, b));
        }
        return want;
    }
    int a = c_read(g, rs);
    if (op == OP_blez || op == OP_blezl || op == OP_bgtz || op == OP_bgtzl) {
        I1(CMPPL(a));
        return op == OP_bgtz || op == OP_bgtzl;
    }
    if (op == OP_bltz || op == OP_bltzl || op == OP_bgez || op == OP_bgezl) {
        I1(CMPPZ(a));
        return op == OP_bgez || op == OP_bgezl;
    }
    fail("%08x: no branch template for %s", (unsigned)ins->vram, name);
    return 0;
}

static int idle_loop(const Insn *ins, const Insn *slot, uint32_t target) {
    return target == ins->vram && (!slot || slot->raw == 0);
}

// A call or tail jump into a range several overlays share: t_lookup picks the loaded one at run time
static void dispatch(FT *t, uint32_t target, int tail, int abi) {
    if (tail) {
        store_all(t);
    } else {
        before_call(t, abi);
        drop(t);
    }
    const_(4, target);
    em_lit(1, V_NAME, N_LOOKUP);
    I2(JSR(1), NOP);
    if (tail) {
        I1(LDSL_PR_POP(15));
        I2(JMP(0), NOP);
        em_barrier();
        return;
    }
    I2(JSR(0), NOP);
    after_call(t, abi);
}

static void leave_to(FT *t, uint32_t target) {
    if (t->lo <= target && target < t->hi) {
        em_br(0, lab(t, target));
    } else {
        store_all(t);
        I1(LDSL_PR_POP(15));
        em_lit(1, V_FUNC, target);
        I2(JMP(1), NOP);
        em_barrier();
    }
}


static int writes_reg(const Insn *i, int r) {
    int id = i->ri.uniqueId;
    if (r == 31 && (i->op == OP_jal || id == RABBITIZER_INSTR_ID_cpu_bgezal || id == RABBITIZER_INSTR_ID_cpu_bltzal ||
                    id == RABBITIZER_INSTR_ID_cpu_bgezall || id == RABBITIZER_INSTR_ID_cpu_bltzall ||
                    (i->op == OP_jalr && ((i->raw >> 11) & 31) == 31)))
        return 1;
    return (RabbitizerInstrDescriptor_modifiesRt(D(i)) && (int)((i->raw >> 16) & 31) == r) ||
           (RabbitizerInstrDescriptor_modifiesRd(D(i)) && (int)((i->raw >> 11) & 31) == r);
}

// reg only ever holds the entry $ra (a move from $ra before anything writes $ra), so jr reg returns
static int ra_alias(const FT *t, int reg) {
    uint32_t first_ra = t->hi;
    for (uint32_t k = 0; k < t->n; k++)
        if (writes_reg(&t->ins[k], 31) && t->ins[k].vram < first_ra)
            first_ra = t->ins[k].vram;
    int copies = 0;
    for (uint32_t k = 0; k < t->n; k++) {
        const Insn *i = &t->ins[k];
        if (!writes_reg(i, reg))
            continue;
        uint32_t a = (i->raw >> 21) & 31, b = (i->raw >> 16) & 31;
        int mv = i->op == OP_or || i->op == OP_addu || i->ri.uniqueId == RABBITIZER_INSTR_ID_cpu_daddu;
        if (!mv || !((a == 0 && b == 31) || (a == 31 && b == 0)) || i->vram >= first_ra)
            return 0;
        copies++;
    }
    return copies > 0;
}

// bal: a call with no ABI on either side, so every register goes through the context both ways
static void raw_call(FT *t, const Insn *ins, const Insn *slot) {
    uint32_t target = branch_vram(ins);
    if (!is_func(target))
        fail("%08x: bal to %08x, which is not a function", (unsigned)ins->vram, (unsigned)target);
    if (slot)
        emit_slot(t, slot);
    int d = c_write(&t->g, 31);
    const_(d, ov_real(ins->vram) + 8);
    c_post_op(&t->g);
    t->g.pinned = 0;
    before_call(t, 0);
    drop(t);
    em_lit(1, V_FUNC, target);
    I2(JSR(1), NOP);
    after_call(t, 0);
}

// j and jal: overlay targets dispatch, externals go through C, the rest jump or call directly
static void ctl_jump(FT *t, const Insn *ins, const Insn *slot, int callee_raw) {
    Cache *g = &t->g;
    int many;
    uint32_t raw_target = index_vram(ins), target = ov_resolve(t->func, raw_target, &many);
    if (slot)
        emit_slot(t, slot);
    if (ins->op == OP_j) {
        sync_(t);
        if (idle_loop(ins, slot, target))
            call_c(t, N_IDLE, 0, 0, 0);
        if (many || far_overlay(target, many))
            dispatch(t, raw_target, 1, 1);
        else
            leave_to(t, target);
        return;
    }
    int d = c_write(g, 31);
    const_(d, ov_real(ins->vram) + 8);
    c_post_op(g);
    g->pinned = 0;
    if (many || far_overlay(target, many)) {
        dispatch(t, raw_target, 0, !callee_raw);
        return;
    }
    if (!is_func(target) && ext_raw(target) < 0) {
        call_c(t, N_EXTERNAL, 1, raw_target, 0);
        return;
    }
    before_call(t, !callee_raw);
    drop(t);
    em_lit(1, V_FUNC, target);
    I2(JSR(1), NOP);
    after_call(t, !callee_raw);
}

// jr through a register the scan found no table for: a tail jump to its target, looked up at run time
static void jr_lookup(FT *t, int rs, const Insn *slot) {
    Cache *g = &t->g;
    int tr = c_read(g, rs);
    I1(MOVL_PUSH(tr, 15));
    g->pinned = 0;
    if (slot)
        emit_slot(t, slot);
    c_post_op(g);
    g->pinned = 0;
    store_all(t);
    I1(MOVL_POP(15, 4));
    em_lit(1, V_NAME, N_LOOKUP);
    I2(JSR(1), NOP);
    I1(LDSL_PR_POP(15));
    I2(JMP(0), NOP);
    em_barrier();
}

// jr: a return, a jump through the function's jump table, or else a tail jump looked up at run time
static void ctl_jr(FT *t, const Insn *ins, const Insn *slot) {
    Cache *g = &t->g;
    int rs = (ins->raw >> 21) & 31;
    if (rs == 31 || ra_alias(t, rs)) {
        if (slot)
            emit_slot(t, slot);
        leave_function(t);
        I1(LDSL_PR_POP(15));
        I2(RTS, NOP);
        em_barrier();
        return;
    }
    int ti = ft_table(t, ins->vram);
    if (ti < 0) {
        jr_lookup(t, rs, slot);
        return;
    }
    if (slot && slot->raw != 0)
        fail("%08x: jump-table jr with a non-empty slot", (unsigned)ins->vram);
    drop(t);
    if (g->has_fixed & BIT(rs))
        I1(MOV(g->fixed[rs], 1));
    else
        I1(rs < 16 ? MOVL_LDD(4 * rs, 14, 1) : MOVL_LDD(4 * (rs - 16), 12, 1));
    const_(2, ov_real(t->lo));
    I1(SUB(2, 1));
    I1(MOV(1, 0));
    em_lit(2, V_LABEL, t->tab_label[ti]);
    I1(MOVL_LD0(2, 1));
    I2(JMP(1), NOP);
    em_barrier();
}

// jalr: the target looked up at run time
static void ctl_jalr(FT *t, const Insn *ins, const Insn *slot, int abi_point, int callee_raw) {
    Cache *g = &t->g;
    int rs = (ins->raw >> 21) & 31, rd = (ins->raw >> 11) & 31;
    if (!abi_point)
        sync_(t);
    int tr = c_read(g, rs);
    I1(MOVL_PUSH(tr, 15));
    g->pinned = 0;
    if (slot)
        emit_slot(t, slot);
    int d = c_write(g, rd);
    const_(d, ov_real(ins->vram) + 8);
    c_post_op(g);
    g->pinned = 0;
    before_call(t, !callee_raw);
    drop(t);
    I1(MOVL_POP(15, 4));
    em_lit(1, V_NAME, N_LOOKUP);
    I2(JSR(1), NOP);
    I2(JSR(0), NOP);
    after_call(t, !callee_raw);
}

static void ctl_branch(FT *t, const Insn *ins, const Insn *slot);

static void control(FT *t, const Insn *ins, const Insn *slot) {
    int op = ins->op;
    Cache *g = &t->g;
    int rs31 = ((ins->raw >> 21) & 31) == 31;
    int abi_point = t->level >= 2 && (op == OP_jal || op == OP_jalr || (op == OP_jr && rs31));
    int many = 0;
    uint32_t key = op == OP_jal ? ov_resolve(t->func, index_vram(ins), &many) : 0;
    int ck = op == OP_jal ? find_u32(M.start, M.nfunc, key) : -1;
    int callee_raw = ck >= 0 && !many && M.raw[ck];  // it takes registers no ABI call passes
    if (op == OP_jal && ck < 0 && M.lazy)
        callee_raw = far_overlay(key, many) ? far_raw(index_vram(ins)) : ext_raw(key) > 0;
    if (t->keep && (op == OP_jal || op == OP_jalr))
        callee_raw = 1;
    if (callee_raw)
        abi_point = 0;
    if (!abi_point)
        sync_(t);
    if (is_bal(ins->raw)) {
        raw_call(t, ins, slot);
    } else if (is_getpc(ins->raw)) {
        int d = c_write(g, 31);
        const_(d, ov_real(ins->vram) + 8);
        c_post_op(g);
        if (slot)
            emit_slot(t, slot);
        sync_(t);
    } else if (op == OP_jal || op == OP_j) {
        ctl_jump(t, ins, slot, callee_raw);
    } else if (op == OP_jr) {
        ctl_jr(t, ins, slot);
    } else if (op == OP_jalr) {
        ctl_jalr(t, ins, slot, abi_point, callee_raw);
    } else {
        ctl_branch(t, ins, slot);
    }
}

// The conditional branches, likely or not, with their delay slot
static void ctl_branch(FT *t, const Insn *ins, const Insn *slot) {
    Cache *g = &t->g;
    uint32_t target = branch_vram(ins);
    int likely = RabbitizerInstrDescriptor_isBranchLikely(D(ins));
    int taken = cond(t, ins);
    g->pinned = 0;
    t->f.pinned = 0;
    if (taken == C_NEVER) {
        if (!likely && slot) {
            emit_slot(t, slot);
            sync_(t);
        }
        return;
    }
    if (taken == C_ALWAYS) {
        if (slot)
            emit_slot(t, slot);
        sync_(t);
        if (idle_loop(ins, slot, target))
            call_c(t, N_IDLE, 0, 0, 0);
        leave_to(t, target);
        return;
    }
    if (idle_loop(ins, slot, target)) {  // GCC's hoisted while (x != y);: nothing in the loop changes its operands
        uint32_t skip = new_label();
        em_br(taken ? 'f' : 't', skip);
        sync_(t);
        call_c(t, N_IDLE, 0, 0, 0);
        leave_to(t, target);
        em_label(skip);
        drop(t);
        return;
    }
    if (likely) {
        uint32_t skip = new_label();
        em_br(taken ? 'f' : 't', skip);
        if (slot)
            emit_slot(t, slot);
        sync_(t);
        leave_to(t, target);
        em_label(skip);
        drop(t);
        return;
    }
    if (slot && slot->raw != 0) {
        uint32_t mark = E.n;
        I1(MOVT(1));
        I1(MOVL_PUSH(1, 15));
        int clobbered = emit_slot(t, slot);
        sync_(t);
        if (clobbered) {
            I1(MOVL_POP(15, 1));
            I1(TST(1, 1));
            taken = !taken;
        } else {
            memmove(E.it + mark, E.it + mark + 2, (E.n - mark - 2) * sizeof *E.it);
            E.n -= 2;
        }
    }
    if (!(t->lo <= target && target < t->hi)) {
        if (!is_func(target))
            fail("%08x: conditional branch out of the function", (unsigned)ins->vram);
        uint32_t skip = new_label();  // a conditional tail into another function (shared epilogues)
        em_br(taken ? 'f' : 't', skip);
        sync_(t);
        leave_to(t, target);
        em_label(skip);
        drop(t);
        return;
    }
    em_br(taken ? 't' : 'f', lab(t, target));
}

static void kseg1_access(FT *t) {
    uint32_t k = 0;
    t->mmio = 0;
    for (uint32_t i = 0; i < t->n; i++) {
        const Insn *ins = &t->ins[i];
        uint32_t raw = ins->raw;
        int rs = (raw >> 21) & 31, rt = (raw >> 16) & 31, rd = (raw >> 11) & 31;
        int op = ins->op;
        if ((op_flags[op] & F_MEM) && (k & (1u << rs))) {
            t->mmio = 1;
            return;
        }
        if (op == OP_lui) {
            uint32_t imm = raw & 0xFFFF;
            if (imm >= 0xA000 && imm < 0xC000) k |= 1u << rt; else k &= ~(1u << rt);
        } else if (op == OP_addiu || op == OP_ori) {
            if (k & (1u << rs)) k |= 1u << rt; else k &= ~(1u << rt);
        } else if (op == OP_or || op == OP_addu) {
            if (k & ((1u << rs) | (1u << rt))) k |= 1u << rd; else k &= ~(1u << rd);
        } else {
            if (RabbitizerInstrDescriptor_modifiesRt(D(ins))) k &= ~(1u << rt);
            if (RabbitizerInstrDescriptor_modifiesRd(D(ins))) k &= ~(1u << rd);
        }
    }
}

// Python dict semantics: counts keyed by register, iterated in first-insertion order
typedef struct {
    int n;
    int8_t key[64], at[64];
    int32_t cnt[64];
} Use;

static void use_init(Use *u) {
    u->n = 0;
    memset(u->at, -1, sizeof u->at);
}

static void use_add(Use *u, int r) {
    if (u->at[r] < 0) {
        u->at[r] = u->n;
        u->key[u->n] = r;
        u->cnt[u->n++] = 0;
    }
    u->cnt[(int)u->at[r]]++;
}

static int use_get(const Use *u, int r) { return u->at[r] < 0 ? 0 : u->cnt[(int)u->at[r]]; }

static int cost(int r, uint64_t rd, uint64_t wr, uint64_t store, uint64_t reload, uint64_t ex, int unit, int mcalls,
                int ccalls) {
    int inr = !!(rd & BIT(r)), inw = !!(wr & BIT(r));
    int c = inr + (inw && (ex & BIT(r)));
    c += mcalls * ((inr && (reload & BIT(r))) + (inw && (store & BIT(r))));
    c += ccalls * (inr + inw);
    return unit * c;
}

static int pick(const Use *u, uint64_t rd, uint64_t wr, uint64_t store, uint64_t reload, uint64_t ex, int unit,
                int limit, int mcalls, int ccalls, int8_t *out) {
    int key[64], gain[64], n = u->n;
    for (int i = 0; i < n; i++) {
        key[i] = u->key[i];
        gain[i] = u->cnt[i] - cost(u->key[i], rd, wr, store, reload, ex, unit, mcalls, ccalls);
    }
    for (int i = 1; i < n; i++) {
        int kk = key[i], gg = gain[i], j = i - 1;
        while (j >= 0 && -gain[j] > -gg) {
            key[j + 1] = key[j], gain[j + 1] = gain[j];
            j--;
        }
        key[j + 1] = kk, gain[j + 1] = gg;
    }
    int m = 0;
    for (int i = 0; i < n && m < limit; i++)
        if (gain[i] >= 2)
            out[m++] = key[i];
    return m;
}

// What a function does with its registers: uses, reads and writes of each, word base registers, and calls
typedef struct {
    Use guse, fuse, wbase, bwrites;
    uint64_t gwritten, gread, fwritten, fread;
    int mcalls, ccalls, sp_hb;
} Usage;

static void use_fprs(Usage *u, const Insn *ins) {
    const RabbitizerInstrDescriptor *d = D(ins);
    uint32_t raw = ins->raw;
    int fr[3] = {(int)((raw >> 11) & 31), (int)((raw >> 16) & 31), (int)((raw >> 6) & 31)};
    int pairs = ins->op == OP_ldc1 || ins->op == OP_sdc1;
    int fused[3] = {RabbitizerInstrDescriptor_readsFs(d), RabbitizerInstrDescriptor_readsFt(d),
                    RabbitizerInstrDescriptor_readsFd(d)};
    int fmod[3] = {RabbitizerInstrDescriptor_modifiesFs(d), RabbitizerInstrDescriptor_modifiesFt(d),
                   RabbitizerInstrDescriptor_modifiesFd(d)};
    for (int k = 0; k < 3; k++)
        if (fused[k])
            for (int q = 0; q < 1 + pairs; q++) {
                use_add(&u->fuse, fr[k] + q);
                u->fread |= BIT(fr[k] + q);
            }
    for (int k = 0; k < 3; k++)
        if (fmod[k])
            for (int q = 0; q < 1 + pairs; q++) {
                use_add(&u->fuse, fr[k] + q);
                u->fwritten |= BIT(fr[k] + q);
            }
}

static void use_insn(const FT *t, Usage *u, const Insn *ins) {
    uint32_t raw = ins->raw;
    int op = ins->op;
    int rs = (raw >> 21) & 31, rt = (raw >> 16) & 31, rd = (raw >> 11) & 31;
    if ((op == OP_lb || op == OP_lbu || op == OP_lh || op == OP_lhu || op == OP_sb || op == OP_sh) && rs == 29)
        u->sp_hb = 1;
    if (raw == 0)
        return;
    if (op == OP_jal) {
        uint32_t k = jump_key(t->func, ins);
        if (is_func(k) || (ext_raw(k) >= 0 && !far_overlay(k, 0))) u->mcalls++;
        else u->ccalls++;
    }
    int fmt = raw >> 26 == 0x11 ? (int)((raw >> 21) & 31) : -1;
    if ((op_flags[op] & F_GENERIC) || fmt == 0x11 || (fmt == 0x14 && op != OP_cvt_s_w) ||
        (t->mmio && (op_flags[op] & F_MEM) && rs != 29))
        u->ccalls++;
    const RabbitizerInstrDescriptor *d = D(ins);
    int rused[3] = {RabbitizerInstrDescriptor_readsRs(d), RabbitizerInstrDescriptor_readsRt(d),
                    RabbitizerInstrDescriptor_readsRd(d)};
    int rmod[3] = {RabbitizerInstrDescriptor_modifiesRs(d), RabbitizerInstrDescriptor_modifiesRt(d),
                   RabbitizerInstrDescriptor_modifiesRd(d)};
    int rr[3] = {rs, rt, rd};
    for (int k = 0; k < 3; k++)
        if (rused[k] && rr[k]) {
            use_add(&u->guse, rr[k]);
            u->gread |= BIT(rr[k]);
        }
    for (int k = 0; k < 3; k++)
        if (rmod[k] && rr[k]) {
            use_add(&u->guse, rr[k]);
            u->gwritten |= BIT(rr[k]);
            use_add(&u->bwrites, rr[k]);
        }
    if (RabbitizerInstrDescriptor_doesLink(d))
        u->gwritten |= BIT(31);
    if (op == OP_lw || op == OP_sw || op == OP_lwc1 || op == OP_swc1 || op == OP_ldc1 || op == OP_sdc1)
        use_add(&u->wbase, rs);
    use_fprs(u, ins);
}

// $sp when it is a base often enough or takes halfwords and bytes, and the busiest base the function rarely writes
static int pick_shadows(const Usage *u, int *shadows) {
    int nsh = 0;
    if (use_get(&u->wbase, 29) >= 2 || u->sp_hb)
        shadows[nsh++] = 29;
    int best_n = -1, best_r = -1;
    for (int i = 0; i < u->wbase.n; i++) {
        int r = u->wbase.key[i], n = u->wbase.cnt[i];
        if (r == 0 || r == 29 || n < 4 || use_get(&u->bwrites, r) * 2 > n)
            continue;
        if (n > best_n || (n == best_n && r > best_r))
            best_n = n, best_r = r;
    }
    if (best_r >= 0)
        shadows[nsh++] = best_r;
    return nsh;
}

static void allocate(FT *t) {
    Usage u;  // only the scalars: use_init sets what the Use tables need, and zeroing them costs 1.6 KB a function
    u.gwritten = u.gread = u.fwritten = u.fread = 0, u.mcalls = u.ccalls = u.sp_hb = 0;
    use_init(&u.guse), use_init(&u.fuse), use_init(&u.wbase), use_init(&u.bwrites);
    for (uint32_t i = 0; i < t->n; i++)
        use_insn(t, &u, &t->ins[i]);
    int shadows[2], nsh = pick_shadows(&u, shadows);
    uint64_t gwritten = u.gwritten, gread = u.gread, fwritten = u.fwritten, fread = u.fread;
    int mcalls = u.mcalls, ccalls = u.ccalls;
    int8_t fixed[64];
    int nfixed = pick(&u.guse, gread, gwritten, G_STORE, G_RELOAD, G_EXIT, 1, 8 - 3 - nsh, mcalls, ccalls, fixed);
    int regs[8], nregs = 8;
    memcpy(regs, (int[]){3, 4, 5, 6, 7, 8, 9, 10}, sizeof regs);
    for (int i = 0; i < nsh; i++) {
        t->g.shadow[shadows[i]] = regs[--nregs];
        t->g.has_shadow |= BIT(shadows[i]);
    }
    for (int i = 0; i < nfixed; i++) {
        t->g.fixed[fixed[i]] = regs[--nregs];
        t->g.has_fixed |= BIT(fixed[i]);
    }
    t->g.fixed_written = gwritten, t->g.fixed_read = gread;
    int fregs[12], nf = 12;
    for (int i = 0; i < 12; i++) fregs[i] = 4 + i;
    int8_t ffixed[64];
    int nff = pick(&u.fuse, fread, fwritten, F_STORE, F_RELOAD, F_EXIT, 2, 12 - 4, mcalls, ccalls, ffixed);
    for (int i = 0; i < nff; i++) {
        t->f.fixed[ffixed[i]] = fregs[--nf];
        t->f.has_fixed |= BIT(ffixed[i]);
    }
    t->f.fixed_written = fwritten, t->f.fixed_read = fread;
}

static Insn *g_ins;
static uint32_t g_ins_cap;
static uint8_t *g_marks;
static uint32_t g_marks_cap;

static int reach_mark(FT *t, uint32_t v, uint32_t from) {
    if (v < t->lo || v >= t->hi || (v - t->lo) & 3 || t->reach[(v - t->lo) / 4]) return 0;
    t->reach[(v - t->lo) / 4] = 1;
    return v <= from;  // behind the sweep: another pass picks up what it reaches
}

// Words no path from the entry or a jump table reaches are data inside the scan's hull (a FRAGMENT header after its
// entry's j): they become traps, so nothing needs a template for them. Over-marking is safe, so links fall through
static void find_reach(FT *t) {
    int again = 1;
    t->reach[0] = 1;
    for (uint32_t k = 0; k < t->ntabs; k++) {
        const JTab *jt = &M.tab[t->tabs[k]];
        for (uint32_t i = 0; i < jt->n; i++) reach_mark(t, rd32(jt->targets + 4 * i), t->hi);
    }
    while (again) {
        again = 0;
        for (uint32_t i = 0; i < t->n; i++) {
            const Insn *ins = &t->ins[i];
            uint32_t v = ins->vram, hi16 = ins->raw >> 16;
            if (!t->reach[i]) continue;
            if (isBranch(ins) || isJump(ins)) {
                again |= reach_mark(t, v + 4, v);
                if (isBranch(ins)) again |= reach_mark(t, branch_vram(ins), v);
                if (ins->op == OP_j) again |= reach_mark(t, jump_key(t->func, ins), v);
                int stops = ins->op == OP_j || ins->op == OP_jr || hi16 == 0x1000 || hi16 == 0x0401;
                if (!stops) again |= reach_mark(t, v + 8, v);
                if (i + 1 < t->n && !t->leader[i + 1]) i++;  // the slot runs only as the slot: it doesn't fall through
            } else if (ins->raw != 0x42000018u) {  // eret
                again |= reach_mark(t, v + 4, v);
            }
        }
    }
}
static uint32_t g_tabs[512], g_tab_label[512];

static void decode(uint32_t lo, uint32_t hi) {
    uint32_t n = (hi - lo) / 4;
    g_ins = grow(g_ins, &g_ins_cap, n + 1, sizeof *g_ins);
    for (uint32_t i = 0; i < n; i++) {
        Insn *x = &g_ins[i];
        uint32_t v = lo + 4 * i;
        x->raw = word(v);
        x->vram = v;
        RabbitizerInstruction_init(&x->ri, x->raw, v);
        RabbitizerInstruction_processUniqueId(&x->ri);
        x->op = opmap[x->ri.uniqueId];
    }
}

static void ft_init(FT *t, uint32_t func, uint32_t hi, int level) {
    memset(t, 0, sizeof *t);
    t->level = level;
    t->func = t->lo = func;
    t->hi = hi;
    t->nwords = (hi - func) / 4;
    t->ins = g_ins;
    t->n = t->nwords;
    cache_init(&t->g, 0);
    cache_init(&t->f, 1);
    for (uint32_t i = 0; i <= t->nwords; i++)
        new_label();
    t->tabs = g_tabs, t->tab_label = g_tab_label;
    for (uint32_t i = 0; i < M.ntab; i++)
        if (t->lo <= M.tab[i].jr && M.tab[i].jr < t->hi) {
            if (t->ntabs == 512)
                die("too many jump tables in %08x", (unsigned)func);
            t->tabs[t->ntabs] = i;
            t->tab_label[t->ntabs++] = new_label();
        }
    g_marks = grow(g_marks, &g_marks_cap, 3 * (t->nwords + 1), 1);
    t->leader = g_marks, t->loophead = g_marks + t->nwords + 1, t->reach = g_marks + 2 * (t->nwords + 1);
    memset(g_marks, 0, 3 * (t->nwords + 1));
#define MARK(arr, v) do { if (t->lo <= (v) && (v) < t->hi) (arr)[((v) - t->lo) / 4] = 1; } while (0)
    MARK(t->leader, t->lo);
    for (uint32_t i = 0; i < t->n; i++) {
        const Insn *ins = &t->ins[i];
        if (isBranch(ins)) {
            MARK(t->leader, branch_vram(ins));
            MARK(t->leader, ins->vram + 8);
        } else if (ins->op == OP_j) {
            MARK(t->leader, jump_key(t->func, ins));
            MARK(t->leader, ins->vram + 8);
        } else if (ins->op == OP_jr) {
            MARK(t->leader, ins->vram + 8);
        }
    }
    for (uint32_t k = 0; k < t->ntabs; k++) {
        const JTab *jt = &M.tab[t->tabs[k]];
        for (uint32_t i = 0; i < jt->n; i++)
            MARK(t->leader, rd32(jt->targets + 4 * i));
    }
    for (uint32_t i = 0; i < t->n; i++) {
        const Insn *ins = &t->ins[i];
        uint32_t tg;
        if (isBranch(ins)) tg = branch_vram(ins);
        else if (ins->op == OP_j) tg = jump_key(t->func, ins);
        else continue;
        if (t->lo <= tg && tg <= ins->vram)
            MARK(t->loophead, tg);
    }
#undef MARK
    find_reach(t);
    kseg1_access(t);
    if (level >= 2)
        allocate(t);
}

static int is_leader(FT *t, uint32_t v) { return v >= t->lo && v < t->hi && t->leader[(v - t->lo) / 4]; }
static int is_loophead(FT *t, uint32_t v) { return v >= t->lo && v < t->hi && t->loophead[(v - t->lo) / 4]; }

// Whether control can run past the function's last word: the scan sometimes starts a function mid-way through another
static int falls_off(const FT *t) {
    if (!t->n) return 0;
    const Insn *x = &t->ins[t->n - 1];
    if (t->n >= 2 && (isBranch(&t->ins[t->n - 2]) || isJump(&t->ins[t->n - 2]))) x = &t->ins[t->n - 2];
    else if (!isBranch(x) && !isJump(x)) return x->raw != 0x42000018u;  // eret
    if (RabbitizerInstrDescriptor_doesLink(D(x))) return 1;
    return !(x->op == OP_j || x->op == OP_jr || (x->raw >> 16) == 0x1000);
}

static void translate(FT *t) {
    em_flabel();
    I1(STSL_PR_PUSH(15));
    c_load_fixed(&t->g, ALL);
    c_load_fixed(&t->f, ALL);
    uint32_t n = t->n, idx = 0;
    while (idx < n) {
        const Insn *ins = &t->ins[idx];
        if (is_leader(t, ins->vram)) {
            sync_(t);
            drop(t);
            em_label(lab(t, ins->vram));
            if (is_loophead(t, ins->vram))
                backedge_poll(t);
        }
        if (!t->reach[idx]) {  // data: what follows it is reached only through a label
            em_lit(4, V_CONST, ov_real(ins->vram));
            em_lit(1, V_NAME, N_UNTRANSLATED);
            I2(JSR(1), NOP);
            idx++;
            continue;
        }
        if (isBranch(ins) || isJump(ins)) {
            const Insn *slot = idx + 1 < n ? &t->ins[idx + 1] : NULL;
            control(t, ins, slot);
            t->g.pinned = 0;
            t->f.pinned = 0;
            if (slot && is_leader(t, slot->vram)) {
                sync_(t);
                em_br(0, lab(t, slot->vram + 4));
                drop(t);
                em_label(lab(t, slot->vram));
                if (is_loophead(t, slot->vram))
                    backedge_poll(t);
                op_(t, slot);
                after_op(t);
            }
            idx += 2;
            continue;
        }
        op_(t, ins);
        after_op(t);
        idx++;
    }
    if (falls_off(t))
        xl_log("  f_%08X runs on into %08X\n", (unsigned)t->func, (unsigned)t->hi);
    if (falls_off(t) && is_func(t->hi)) {
        leave_to(t, t->hi);
    } else if (falls_off(t)) {
        em_lit(4, V_CONST, t->hi);
        em_lit(1, V_NAME, N_UNTRANSLATED);
        I2(JSR(1), NOP);
    }
    em_barrier();
    for (uint32_t k = 0; k < t->ntabs; k++)
        push_item(IT_TABLE)->a = k;
}

static void emit_hook(uint32_t fn, int hi) {
    const char *hook = M.hook_str[hi];
    em_flabel();
    if (!strcmp(hook, "noop")) {
        I2(RTS, NOP);
    } else if (!strcmp(hook, "unsupported")) {
        em_lit(4, V_CONST, fn);
        em_lit(1, V_NAME, N_UNSUPPORTED);
        I2(JSR(1), NOP);
    } else {
        I1(STSL_PR_PUSH(15));
        I1(MOV(14, 4));
        I1(ADDI(-12, 4));
        em_lit(1, V_NAME, M.hook_name[hi]);
        I2(JSR(1), NOP);
        I1(LDSL_PR_POP(15));
        I2(RTS, NOP);
    }
    em_barrier();
}

static void emit_stub(uint32_t fn) {
    em_flabel();
    em_lit(4, V_CONST, fn);
    em_lit(1, V_NAME, N_UNTRANSLATED);
    I2(JSR(1), NOP);
    em_barrier();
}

typedef struct {
    uint8_t tag;
    uint32_t val, at;
} Pend;

static Pend *P;
static uint32_t nP, capP;
static uint32_t *BR;
static uint32_t nBR, capBR;
static uint32_t *addr, capaddr;

typedef struct {
    int emit;
    uint32_t p, base, func_index, flbl_pos;
    const FT *t;
    const uint8_t *longs;
} Lay;

static uint32_t absoff(const Lay *L, uint32_t pos) { return L->base + pos - L->p; }

static void put16(const Lay *L, uint32_t pos, uint16_t v) {
    uint32_t o = absoff(L, pos);
    O->text = grow(O->text, &O->cap, o + 2, 1);
    O->text[o] = v & 0xFF, O->text[o + 1] = v >> 8;
    if (o + 2 > O->len) O->len = o + 2;
}

static void put32(const Lay *L, uint32_t pos, uint32_t v) {
    put16(L, pos, v & 0xFFFF);
    put16(L, pos + 2, v >> 16);
}

static void add_rel(uint32_t off, int kind, uint32_t id) {
    O->rel = grow(O->rel, &O->caprel, O->nrel + 1, sizeof *O->rel);
    if (O->nrel && O->rel[O->nrel - 1].off >= off)
        die("relocations out of order at %x", (unsigned)off);
    O->rel[O->nrel].off = off;
    O->rel[O->nrel].kind = kind;
    O->rel[O->nrel++].id = id;
}

static int32_t label_at(uint32_t id) {
    if (id >= E.nlab || E.lpos[id] < 0)
        die("undefined label %u", (unsigned)id);
    return E.lpos[id];
}

static void put_value(const Lay *L, uint32_t pos, int tag, uint32_t val) {
    uint32_t o = absoff(L, pos);
    switch (tag) {
    case V_CONST:
        put32(L, pos, val);
        break;
    case V_LABEL:
        put32(L, pos, absoff(L, label_at(val)));
        add_rel(o, XL_SYM_SECTION, 0);
        break;
    case V_NAME:
        put32(L, pos, 0);
        add_rel(o, XL_SYM_UND, und_name(val));
        break;
    case V_FUNC: {
        int fi = find_u32(M.start, M.nfunc, val);
        put32(L, pos, 0);
        if (fi >= 0) add_rel(o, XL_SYM_FUNC, fi);
        else add_rel(o, XL_SYM_UND, und_vram(val));
        break;
    }
    }
}

static void patch_movl(const Lay *L, uint32_t at, uint32_t lit) {
    uint32_t base = (absoff(L, at) + 4) & ~3u, la = absoff(L, lit);
    if (la < base || (la - base) / 4 > 255 || (la - base) & 3)
        die("mov.l literal out of range at %x", (unsigned)absoff(L, at));
    uint32_t o = absoff(L, at);
    uint16_t v = (O->text[o] | O->text[o + 1] << 8) | ((la - base) / 4);
    put16(L, at, v);
}

static void align4(const Lay *L, uint32_t *pos) {
    if (*pos % 4) {
        if (L->emit) put16(L, *pos, NOP);
        *pos += 2;
    }
}

static void flush_pool(const Lay *L, uint32_t *pos, int jump) {
    if (!nP)
        return;
    uint32_t bra_at = *pos;
    if (jump)
        *pos += 4;
    align4(L, pos);
    if (L->emit)
        addr = grow(addr, &capaddr, nP, sizeof *addr);
    for (uint32_t i = 0; i < nP; i++) {
        int dup = -1;
        for (uint32_t j = 0; j < i; j++)
            if (P[j].tag == P[i].tag && P[j].val == P[i].val) {
                dup = j;
                break;
            }
        if (dup >= 0) {
            if (L->emit) addr[i] = addr[dup];
            continue;
        }
        if (L->emit) {
            addr[i] = *pos;
            put_value(L, *pos, P[i].tag, P[i].val);
        }
        *pos += 4;
    }
    if (L->emit) {
        for (uint32_t i = 0; i < nP; i++)
            patch_movl(L, P[i].at, addr[i]);
        if (jump) {
            int32_t disp = (int32_t)(*pos - (bra_at + 4)) / 2;
            put16(L, bra_at, BRA(disp));
            put16(L, bra_at + 2, NOP);
        }
    }
    nP = 0;
}

static void pend(uint8_t tag, uint32_t val, uint32_t at) {
    P = grow(P, &capP, nP + 1, sizeof *P);
    P[nP].tag = tag, P[nP].val = val, P[nP++].at = at;
}

// The pending pool goes out ahead of n more bytes that would put its first literal out of mov.l's reach
static void need_room(const Lay *L, uint32_t *pos, uint32_t first, uint32_t n) {
    if (nP && *pos + n - first > 880) flush_pool(L, pos, 1);
}

// A branch: short while its label is in reach, else a literal-pool jump (L->longs)
static void lay_branch(const Lay *L, const Item *it, uint32_t idx, uint32_t *pos, uint32_t *first) {
    char c = it->x;
    need_room(L, pos, *first, 10);
    BR = grow(BR, &capBR, nBR + 1, 2 * sizeof *BR);
    BR[2 * nBR] = idx, BR[2 * nBR + 1] = *pos;
    nBR++;
    if (!(L->longs[idx / 8] & (1 << idx % 8))) {
        if (L->emit) {
            int32_t disp = (label_at(it->a) - (int32_t)(*pos + 4)) / 2;
            if (c) put16(L, *pos, c == 't' ? BT(disp) : BF(disp));
            else put16(L, *pos, BRA(disp)), put16(L, *pos + 2, NOP);
        }
        *pos += c ? 2 : 4;
    } else {
        if (!nP) *first = *pos;
        pend(V_LABEL, it->a, *pos + (c ? 2 : 0));
        if (c) {
            if (L->emit) put16(L, *pos, c == 't' ? BF(2) : BT(2));
            *pos += 2;
        }
        if (L->emit) put16(L, *pos, MOVL_PC(0, 1)), put16(L, *pos + 2, JMP(1)), put16(L, *pos + 4, NOP);
        *pos += 6;
    }
    if (!c)
        flush_pool(L, pos, 0);
}

// A jump table: one word per instruction of the function, a label where the table can land, else N_BADJUMP
static void lay_table(const Lay *L, const Item *it, uint32_t *pos) {
    flush_pool(L, pos, 0);
    align4(L, pos);
    const FT *t = L->t;
    uint32_t k = it->a;
    const JTab *jt = &M.tab[t->tabs[k]];
    if (L->emit && E.lpos[t->tab_label[k]] != (int32_t)*pos)
        die("table label moved between passes");
    E.lpos[t->tab_label[k]] = *pos;
    for (uint32_t v = t->lo; v < t->hi; v += 4) {
        if (L->emit) {
            int hit = 0;
            for (uint32_t i = 0; i < jt->n && !hit; i++)
                hit = rd32(jt->targets + 4 * i) == v;
            if (hit) put_value(L, *pos, V_LABEL, (v - t->lo) / 4);
            else put_value(L, *pos, V_NAME, N_BADJUMP);
        }
        *pos += 4;
    }
}

// Marks in grew each short branch whose label this pass put out of its reach; returns whether there were any
static int out_of_reach(const Lay *L, uint8_t *grew) {
    int any = 0;
    for (uint32_t i = 0; i < nBR; i++) {
        uint32_t idx = BR[2 * i], bpos = BR[2 * i + 1];
        if (L->longs[idx / 8] & (1 << idx % 8))
            continue;
        const Item *it = &E.it[idx];
        int32_t disp = label_at(it->a) - (int32_t)(bpos + 4);
        int32_t limit = it->x ? 256 : 4096;
        if (!(-limit <= disp && disp < limit - 2)) {
            grew[idx / 8] |= 1 << idx % 8;
            any = 1;
        }
    }
    return any;
}

// One layout pass over the current function's items, starting at an address = p (mod 4); returns the end position
static uint32_t lay(Lay *L, uint8_t *grew, int *any) {
    uint32_t pos = L->p, first = 0;
    nP = 0;
    nBR = 0;
    for (uint32_t idx = 0; idx < E.n; idx++) {
        const Item *it = &E.it[idx];
        switch (it->kind) {
        case IT_LBL:
            if (L->emit && E.lpos[it->a] != (int32_t)pos)
                die("label moved between passes");
            E.lpos[it->a] = pos;
            break;
        case IT_FLBL:
            L->flbl_pos = pos;
            break;
        case IT_I:
            need_room(L, &pos, first, 2 * it->n);
            if (L->emit)
                for (int k = 0; k < it->n; k++)
                    put16(L, pos + 2 * k, item_op(it, k));
            pos += 2 * it->n;
            break;
        case IT_LIT:
            need_room(L, &pos, first, 2);
            if (!nP) first = pos;
            pend(it->tag, it->a, pos);
            if (L->emit) put16(L, pos, MOVL_PC(0, it->x));
            pos += 2;
            break;
        case IT_BR: lay_branch(L, it, idx, &pos, &first); break;
        case IT_BARRIER: flush_pool(L, &pos, 0); break;
        case IT_TABLE: lay_table(L, it, &pos); break;
        }
    }
    flush_pool(L, &pos, 0);
    *any = !L->emit && out_of_reach(L, grew);
    return pos;
}

#define MAXSEQ 64
static uint8_t seq_in[MAXSEQ], seq_out[MAXSEQ];
static uint32_t nseq_in, nseq_out;
static uint8_t *longs, *grewb;
static uint32_t caplongs;

static void layout_and_emit(const FT *t, uint32_t fi, XlStats *st) {
    uint64_t t0 = xl_now_us();
    uint32_t nb = E.n / 8 + 1;
    longs = grow(longs, &caplongs, 2 * nb, 1);
    grewb = longs + nb;
    memset(longs, 0, 2 * nb);
    Lay L = {0};
    L.t = t, L.longs = longs;
    uint32_t k = 0;
    for (;;) {
        L.p = seq_in[k < nseq_in ? k : nseq_in - 1];
        L.base = L.p;
        int any;
        memset(grewb, 0, nb);
        uint32_t end = lay(&L, grewb, &any);
        if (k >= MAXSEQ - 1)
            die("layout did not converge");
        seq_out[k] = end % 4;
        for (uint32_t i = 0; i < nb; i++)
            longs[i] |= grewb[i];
        if (k + 1 >= nseq_in && !any)
            break;
        k++;
    }
    nseq_out = k + 1;
    if (nseq_out > st->iterations)
        st->iterations = nseq_out;
    uint64_t t1 = xl_now_us();
    L.emit = 1;
    L.base = O->len;
    if (L.base % 4 != L.p)
        die("start parity mismatch");
    lay(&L, grewb, &(int){0});
    O->func_off[fi] = L.base + L.flbl_pos - L.p;
    memcpy(seq_in, seq_out, nseq_out);
    nseq_in = nseq_out;
    st->us_layout += t1 - t0;
    st->us_emit += xl_now_us() - t1;
}

static void emit_tail(void) {
    Lay L = {0};
    L.emit = 1;
    L.p = seq_in[nseq_in - 1];
    L.base = O->len;
    uint32_t pos = L.p;
    align4(&L, &pos);
    O->functab_off = absoff(&L, pos);
    for (uint32_t i = 0; i < M.nfunc; i++) {
        put32(&L, pos, M.start[i]);
        put32(&L, pos + 4, 0);
        add_rel(absoff(&L, pos + 4), XL_SYM_FUNC, i);
        pos += 8;
    }
    O->functab_count_off = absoff(&L, pos);
    put32(&L, pos, M.nfunc);
    pos += 4;
    O->call_off = absoff(&L, pos);
    static const uint16_t pre[] = {
        MOVL_PUSH(8, 15), MOVL_PUSH(9, 15), MOVL_PUSH(10, 15), MOVL_PUSH(11, 15), MOVL_PUSH(12, 15),
        MOVL_PUSH(13, 15), MOVL_PUSH(14, 15), STSL_PR_PUSH(15), FMOVS_PUSH(12, 15), FMOVS_PUSH(13, 15),
        FMOVS_PUSH(14, 15), FMOVS_PUSH(15, 15), MOV(5, 14), ADDI(12, 14), MOV(14, 12), ADDI(64, 12), MOV(14, 11),
        ADDI(64, 11), ADDI(64, 11)};
    static const uint16_t post[] = {
        MOVL_LDI(13, 13), JSR(4), NOP, FMOVS_POP(15, 15), FMOVS_POP(15, 14), FMOVS_POP(15, 13), FMOVS_POP(15, 12),
        LDSL_PR_POP(15), MOVL_POP(15, 14), MOVL_POP(15, 13), MOVL_POP(15, 12), MOVL_POP(15, 11), MOVL_POP(15, 10),
        MOVL_POP(15, 9), RTS, MOVL_POP(15, 8)};
    for (size_t i = 0; i < sizeof pre / 2; i++, pos += 2)
        put16(&L, pos, pre[i]);
    uint32_t bias_movl = pos;
    put16(&L, pos, MOVL_PC(0, 13));
    pos += 2;
    for (size_t i = 0; i < sizeof post / 2; i++, pos += 2)
        put16(&L, pos, post[i]);
    align4(&L, &pos);
    put_value(&L, pos, V_NAME, N_BIAS);
    patch_movl(&L, bias_movl, pos);
    pos += 4;
}

volatile uint32_t xl_funcs_done;

void xl_release(void) {
    free_meta();
    void *v[] = {E.it, E.ops, E.lpos, g_ins, g_marks, P, BR, addr, longs};
    for (size_t i = 0; i < sizeof v / sizeof *v; i++) xl_free(v[i]);
    memset(&E, 0, sizeof E);
    g_ins = NULL, g_marks = NULL, P = NULL, BR = NULL, addr = NULL, longs = grewb = NULL;
    g_ins_cap = g_marks_cap = capP = capBR = capaddr = caplongs = 0;
    nP = nBR = 0;
}

void xl_free_out(XlOut *o) {
    for (uint32_t i = 0; i < o->nund; i++) xl_free(o->und[i]);
    void *v[] = {o->text, o->rel, o->und, (void *)o->funcs, o->func_off, o->alias_func, o->ovtab};
    for (size_t i = 0; i < sizeof v / sizeof *v; i++) xl_free(v[i]);
    memset(o, 0, sizeof *o);
}

int xl_run(const uint8_t *meta, size_t len, int level, XlOut *out, XlStats *st) {
    memset(st, 0, sizeof *st);
    xl_funcs_done = 0;
    O = out;
    load_meta(meta, len);
    init_ops();
    out->nfuncs = M.nfunc;
    uint32_t *funcs = xl_alloc(M.nfunc * 4 + 4);  // a copy: the next run's load_meta frees M.start
    memcpy(funcs, M.start, M.nfunc * 4);
    out->funcs = funcs;
    out->novl = 0;
    out->ovtab = xl_alloc(16 * M.novl + 4);
    for (uint32_t i = 0; i < M.nseg; i++) {
        const Seg *s = &M.seg[i];
        if (!is_ovl(s)) continue;
        uint32_t *e = out->ovtab + 4 * out->novl++;
        e[0] = ov_real(s->vram), e[1] = s->size, e[2] = s->rom, e[3] = s->vram;
    }
    out->func_off = xl_alloc(M.nfunc * 4 + 4);
    out->nalias = M.nentry;
    out->alias_name = M.entry_name;
    out->alias_func = xl_alloc(M.nentry * 4 + 4);
    for (uint32_t i = 0; i < M.nentry; i++) {
        int fi = find_u32(M.start, M.nfunc, M.entry_addr[i]);
        if (fi < 0)
            die("entry %s at %08x is not a function", M.entry_name[i], (unsigned)M.entry_addr[i]);
        out->alias_func[i] = fi;
    }
    raw_pass();
    keep_pass();
    seq_in[0] = 0;
    nseq_in = 1;
    for (uint32_t fi = 0; fi < M.nfunc; fi++) {
        uint32_t fn = M.start[fi], hi = M.end[fi];
        uint64_t t0 = xl_now_us();
        st->mips += (hi - fn) / 4;
        em_reset();
        int hk = -1;
        for (uint32_t h = 0; h < M.nhook && hk < 0; h++)
            if (M.hook_addr[h] == fn)
                hk = h;
        static FT t;
        t.ntabs = 0;
        if (hk >= 0) {
            emit_hook(fn, hk);
            const char *s = M.hook_str[hk];
            if (!strcmp(s, "noop")) st->hook_noop++;
            else if (!strcmp(s, "unsupported")) st->hook_unsupported++;
            else st->hook_native++;
        } else {
            jmp_buf jb;
            decode(fn, hi);
            if (!setjmp(jb)) {
                fail_jb = &jb;
                ft_init(&t, fn, hi, level);
                t.raw = M.raw[fi];
                t.keep = M.keep[fi];
                translate(&t);
                st->generic += t.generic;
            } else {
                em_reset();
                emit_stub(fn);
                st->stubs++;
                xl_log("  stub f_%08X: %s\n", (unsigned)fn, fail_msg);
            }
            fail_jb = NULL;
        }
        if (E.n > st->max_items) st->max_items = E.n;
        if (E.nops > st->max_ops) st->max_ops = E.nops;
        st->us_translate += xl_now_us() - t0;
        layout_and_emit(&t, fi, st);
        xl_funcs_done = fi + 1;
    }
    emit_tail();
    return 0;
}
