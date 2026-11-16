#include "tmpl.h"
#include "sim.h"
#include "view.h"
#include "rcp.h"
#include "ovl.h"
#include "os.h"


static void die(const char *what, uint32_t a, uint32_t b) {
    sim_puts(what);
    sim_hex(" ", a);
    sim_hex("", b);
    sim_puts("\n");
    sim_exit(97);
}

static double getd(struct tctx *c, int r) {
    union { double d; uint32_t w[2]; } u;
    u.w[0] = c->fpr[r];
    u.w[1] = c->fpr[r + 1];
    return u.d;
}

static void setd(struct tctx *c, int r, double v) {
    union { double d; uint32_t w[2]; } u;
    u.d = v;
    c->fpr[r] = u.w[0];
    c->fpr[r + 1] = u.w[1];
}

static float getf(struct tctx *c, int r) {
    union { uint32_t u; float f; } x = { c->fpr[r] };
    return x.f;
}

static void setf(struct tctx *c, int r, float v) {
    union { float f; uint32_t u; } x = { v };
    c->fpr[r] = x.u;
}

static int64_t round_even(double v) {
    double f = __builtin_floor(v), d = v - f;
    int64_t i = (int64_t)f;
    if (d > 0.5 || (d == 0.5 && (i & 1))) i++;
    return i;
}

// cvt.w and cvt.l round by FCSR's RM: nearest even, toward zero, toward +inf, toward -inf
static int64_t cvt_rm(const struct tctx *c, double v) {
    switch (c->fcsr & 3) {
    case 1: return (int64_t)v;
    case 2: return (int64_t)__builtin_ceil(v);
    case 3: return (int64_t)__builtin_floor(v);
    default: return round_even(v);
    }
}

// COP1 S/D/W/L ops the switch below leaves out: GCC code reaches far more of the FPU than IDO's
static int fpu_more(struct tctx *c, uint32_t fmt, uint32_t fn, int fs, int ft, int fd) {
    if (fmt == 0x10 || fmt == 0x11) {
        int dbl = fmt == 0x11;
        double a = dbl ? getd(c, fs) : getf(c, fs), b = dbl ? getd(c, ft) : getf(c, ft), r;
        if (fn >= 0x30) {  // c.cond: bit 0 unordered, bit 1 equal, bit 2 less
            int un = a != a || b != b;
            c->fcond = ((fn & 1) && un) || ((fn & 2) && !un && a == b) || ((fn & 4) && !un && a < b);
            return 1;
        }
        switch (fn) {
        case 0x00: r = a + b; break;
        case 0x01: r = a - b; break;
        case 0x02: r = a * b; break;
        case 0x03: r = a / b; break;
        case 0x04: r = __builtin_sqrt(a); break;
        case 0x05: r = __builtin_fabs(a); break;
        case 0x06: r = a; break;
        case 0x07: r = -a; break;
        case 0x0C: c->fpr[fd] = (uint32_t)round_even(a); return 1;
        case 0x0D: c->fpr[fd] = (uint32_t)(int32_t)a; return 1;
        case 0x0E: c->fpr[fd] = (uint32_t)(int32_t)__builtin_ceil(a); return 1;
        case 0x0F: c->fpr[fd] = (uint32_t)(int32_t)__builtin_floor(a); return 1;
        case 0x20: setf(c, fd, (float)a); return 1;
        case 0x21: setd(c, fd, a); return 1;
        case 0x24: c->fpr[fd] = (uint32_t)cvt_rm(c, a); return 1;
        case 0x25: {
            int64_t v = cvt_rm(c, a);
            c->fpr[fd] = (uint32_t)v, c->fpr[fd + 1] = (uint32_t)((uint64_t)v >> 32);
            return 1;
        }
        default: return 0;
        }
        if (dbl) setd(c, fd, r);
        else setf(c, fd, (float)r);
        return 1;
    }
    if (fmt == 0x15 && (fn == 0x20 || fn == 0x21)) {  // cvt.s.l, cvt.d.l
        int64_t v = (int64_t)((uint64_t)c->fpr[fs + 1] << 32 | c->fpr[fs]);
        if (fn == 0x20) setf(c, fd, (float)v);
        else setd(c, fd, (double)v);
        return 1;
    }
    return 0;
}

static void wr(struct tctx *c, int r, uint32_t v) {
    if (r) c->gpr[r] = v;
}

// KSEG1 (0xA0000000-0xBFFFFFFF) goes to the virtual RCP, which also serves the uncached RDRAM alias
static uint32_t ld(uint32_t a, int n) {
    if ((a >> 29) == 5) return rcp_rd(a & 0x1FFFFFFFu, n);
    return n == 4 ? M32(a) : n == 2 ? M16(a) : M8(a);
}

static void st(uint32_t a, uint32_t v, int n) {
    if ((a >> 29) == 5) rcp_wr(a & 0x1FFFFFFFu, v, n);
    else if (n == 4) M32(a) = v;
    else if (n == 2) M16(a) = (uint16_t)v;
    else M8(a) = (uint8_t)v;
}

// SPECIAL: variable shifts, 64-bit shifts and adds, multiplies and divides; 0 for any other function
__attribute__((always_inline)) static inline int t_special(struct tctx *c, uint32_t raw, uint32_t vram) {
    uint32_t rs = (raw >> 21) & 31, rt = (raw >> 16) & 31, rd = (raw >> 11) & 31, *g = c->gpr;
    switch (raw & 63) {
    case 0x04: wr(c, rd, g[rt] << (g[rs] & 31)); return 1;
    case 0x06: wr(c, rd, g[rt] >> (g[rs] & 31)); return 1;
    case 0x07: wr(c, rd, (uint32_t)((int32_t)g[rt] >> (g[rs] & 31))); return 1;
    case 0x0D: die("break", vram, raw); return 1;
    case 0x14: wr(c, rd, (uint32_t)((uint64_t)g[rt] << (g[rs] & 63))); return 1;
    case 0x16: wr(c, rd, (uint32_t)((uint64_t)g[rt] >> (g[rs] & 63))); return 1;
    case 0x17: wr(c, rd, (uint32_t)((int64_t)(uint64_t)g[rt] >> (g[rs] & 63))); return 1;
    case 0x1A:  // 32-bit: a 64-bit divide is __divdi3, several times the cost
        if (g[rt] == 0) die("div0", vram, raw);
        if (g[rs] == 0x80000000u && g[rt] == 0xFFFFFFFFu) {
            c->lo = 0x80000000u, c->hi = 0;
            return 1;
        }
        c->lo = (uint32_t)((int32_t)g[rs] / (int32_t)g[rt]);
        c->hi = (uint32_t)((int32_t)g[rs] % (int32_t)g[rt]);
        return 1;
    case 0x1B: case 0x1E: case 0x1F:
        if (g[rt] == 0) die("div0", vram, raw);
        c->lo = g[rs] / g[rt];
        c->hi = g[rs] % g[rt];
        return 1;
    case 0x1D: {
        uint64_t r = (uint64_t)g[rs] * (uint64_t)g[rt];
        c->lo = (uint32_t)r;
        c->hi = (uint32_t)(r >> 32);
        return 1;
    }
    case 0x2D: wr(c, rd, g[rs] + g[rt]); return 1;
    case 0x3C: case 0x3E: case 0x3F: wr(c, rd, 0); return 1;
    }
    return 0;
}

// COP1: cfc1/ctc1, and the S, D and W ops IDO code uses in single precision where it can; fpu_more has the rest
__attribute__((always_inline)) static inline int t_cop1(struct tctx *c, uint32_t raw) {
    uint32_t fmt = (raw >> 21) & 31, rt = (raw >> 16) & 31, fn = raw & 63;
    uint32_t ft = rt, fs = (raw >> 11) & 31, fd = (raw >> 6) & 31;
    if (fmt == 0x02) {
        wr(c, rt, fs == 31 ? c->fcsr : 0);
        return 1;
    }
    if (fmt == 0x06) {
        if (fs == 31) c->fcsr = c->gpr[rt];
        return 1;
    }
    if (fmt == 0x10) {
        switch (fn) {
        case 0x04: setf(c, fd, __builtin_sqrtf(getf(c, fs))); return 1;
        case 0x05: setf(c, fd, __builtin_fabsf(getf(c, fs))); return 1;
        case 0x0D: { float v = getf(c, fs); c->fpr[fd] = (uint32_t)(int32_t)v; return 1; }
        case 0x21: setd(c, fd, (double)getf(c, fs)); return 1;
        case 0x24: c->fpr[fd] = (uint32_t)cvt_rm(c, getf(c, fs)); return 1;
        case 0x32: c->fcond = getf(c, fs) == getf(c, ft); return 1;
        case 0x3C: c->fcond = getf(c, fs) < getf(c, ft); return 1;
        case 0x3E: c->fcond = getf(c, fs) <= getf(c, ft); return 1;
        }
    } else if (fmt == 0x11) {
        switch (fn) {
        case 0x00: setd(c, fd, getd(c, fs) + getd(c, ft)); return 1;
        case 0x01: setd(c, fd, getd(c, fs) - getd(c, ft)); return 1;
        case 0x02: setd(c, fd, getd(c, fs) * getd(c, ft)); return 1;
        case 0x03: setd(c, fd, getd(c, fs) / getd(c, ft)); return 1;
        case 0x06: setd(c, fd, getd(c, fs)); return 1;
        case 0x07: setd(c, fd, -getd(c, fs)); return 1;
        case 0x0D: c->fpr[fd] = (uint32_t)(int32_t)getd(c, fs); return 1;
        case 0x20: setf(c, fd, (float)getd(c, fs)); return 1;
        case 0x24: c->fpr[fd] = (uint32_t)cvt_rm(c, getd(c, fs)); return 1;
        case 0x32: c->fcond = getd(c, fs) == getd(c, ft); return 1;
        case 0x3C: c->fcond = getd(c, fs) < getd(c, ft); return 1;
        case 0x3E: c->fcond = getd(c, fs) <= getd(c, ft); return 1;
        }
    } else if (fmt == 0x14) {
        switch (fn) {
        case 0x20: setf(c, fd, (float)(int32_t)c->fpr[fs]); return 1;
        case 0x21: setd(c, fd, (double)(int32_t)c->fpr[fs]); return 1;
        }
    }
    return fpu_more(c, fmt, fn, (int)fs, (int)ft, (int)fd);
}

void t_generic(struct tctx *c, uint32_t raw, uint32_t vram) {
    uint32_t op = raw >> 26, rs = (raw >> 21) & 31, rt = (raw >> 16) & 31;
    int32_t imm = (int16_t)raw;
    uint32_t addr = c->gpr[rs] + imm, *g = c->gpr;
    switch (op) {  // the loads and stores stay in this switch: one jump table for the ops GCC code sends here most
    case 0x00: if (t_special(c, raw, vram)) return; break;
    case 0x10:  // mfc0, mtc0
        if (rs == 0) { wr(c, rt, os_mfc0((raw >> 11) & 31)); return; }
        if (rs == 4) { os_mtc0((raw >> 11) & 31, g[rt]); return; }
        break;
    case 0x11: if (t_cop1(c, raw)) return; break;
    case 0x19: wr(c, rt, g[rs] + imm); return;  // daddiu
    case 0x2F: return;                           // cache
    case 0x22: {
        uint32_t w = M32(addr & ~3u), m = (addr & 3) * 8;
        wr(c, rt, (g[rt] & ~(0xFFFFFFFFu << m)) | (w << m));
        return;
    }
    case 0x26: {
        uint32_t w = M32(addr & ~3u), m = 24 - (addr & 3) * 8;
        wr(c, rt, (g[rt] & ~(0xFFFFFFFFu >> m)) | (w >> m));
        return;
    }
    case 0x2A: {
        uint32_t w = M32(addr & ~3u), m = (addr & 3) * 8;
        M32(addr & ~3u) = (w & ~(0xFFFFFFFFu >> m)) | (g[rt] >> m);
        return;
    }
    case 0x2E: {
        uint32_t w = M32(addr & ~3u), m = 24 - (addr & 3) * 8;
        M32(addr & ~3u) = (w & ~(0xFFFFFFFFu << m)) | (g[rt] << m);
        return;
    }
    case 0x20: wr(c, rt, (uint32_t)(int8_t)ld(addr, 1)); return;
    case 0x21: wr(c, rt, (uint32_t)(int16_t)ld(addr, 2)); return;
    case 0x23: wr(c, rt, ld(addr, 4)); return;
    case 0x24: wr(c, rt, ld(addr, 1)); return;
    case 0x25: wr(c, rt, ld(addr, 2)); return;
    case 0x28: st(addr, g[rt], 1); return;
    case 0x29: st(addr, g[rt], 2); return;
    case 0x2B: st(addr, g[rt], 4); return;
    case 0x31: c->fpr[rt] = ld(addr, 4); return;
    case 0x39: st(addr, c->fpr[rt], 4); return;
    case 0x35: c->fpr[rt] = ld(addr + 4, 4); c->fpr[rt + 1] = ld(addr, 4); return;
    case 0x37: wr(c, rt, ld(addr + 4, 4)); return;
    case 0x3D: st(addr, c->fpr[rt + 1], 4); st(addr + 4, c->fpr[rt], 4); return;
    case 0x3F: st(addr, 0, 4); st(addr + 4, g[rt], 4); return;
    }
    die("t_generic: unhandled", vram, raw);
}

void (*t_call_fn)(void (*fn)(void), struct tctx *ctx);
const uint32_t *t_functab_p;
uint32_t t_functab_n;

#define T_HASH 16384
static uint32_t th_vram[T_HASH], th_fn[T_HASH];

static int th_ready;
static uint32_t th_n;

const uint32_t *t_ovtab_p;
uint32_t t_ovtab_n;
uint32_t (*t_ram_code)(uint32_t vram, uint32_t key);

static uint32_t th_probe(uint32_t key) {
    for (uint32_t h = (key >> 2) & (T_HASH - 1); th_vram[h]; h = (h + 1) & (T_HASH - 1))
        if (th_vram[h] == key) return th_fn[h];
    return 0;
}

void t_lookup_insert(uint32_t key, uint32_t fn) {
    uint32_t h = (key >> 2) & (T_HASH - 1);
    while (th_vram[h] && th_vram[h] != key) h = (h + 1) & (T_HASH - 1);
    if (!th_vram[h] && ++th_n > T_HASH / 4 * 3) die("t_lookup: more functions than the lookup table holds", th_n, T_HASH);
    th_vram[h] = key, th_fn[h] = fn;
}

// Backward-shift deletion: every later entry of the probe run moves into the gap if its home allows
void t_lookup_remove(uint32_t key) {
    uint32_t h = (key >> 2) & (T_HASH - 1);
    while (th_vram[h] && th_vram[h] != key) h = (h + 1) & (T_HASH - 1);
    if (!th_vram[h]) return;
    th_n--;
    for (uint32_t j = h;;) {
        th_vram[h] = 0;
        uint32_t home;
        do {
            j = (j + 1) & (T_HASH - 1);
            if (!th_vram[j]) return;
            home = (th_vram[j] >> 2) & (T_HASH - 1);
        } while (h <= j ? h < home && home <= j : h < home || home <= j);
        th_vram[h] = th_vram[j], th_fn[h] = th_fn[j];
        h = j;
    }
}

uint32_t t_lookup_exact(uint32_t key) { return th_probe(key); }
static uint32_t ov_lo, ov_span;  // the hull of every overlay's range

void t_lookup_init(void) {
    if (th_ready) return;
    th_ready = 1;
    if (!t_call_fn) die("t_lookup: no translated code is loaded", 0, 0);
    if (t_functab_n > T_HASH / 4 * 3) die("t_lookup: more functions than the lookup table holds", t_functab_n, T_HASH);
    for (uint32_t i = 0; i < t_functab_n; i++) {
        uint32_t h = (t_functab_p[2 * i] >> 2) & (T_HASH - 1);
        while (th_vram[h]) h = (h + 1) & (T_HASH - 1);
        th_vram[h] = t_functab_p[2 * i];
        th_fn[h] = t_functab_p[2 * i + 1];
        th_n++;
    }
    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (uint32_t i = 0; i < t_ovtab_n; i++) {
        if (t_ovtab_p[4 * i] < lo) lo = t_ovtab_p[4 * i];
        if (t_ovtab_p[4 * i] + t_ovtab_p[4 * i + 1] > hi) hi = t_ovtab_p[4 * i] + t_ovtab_p[4 * i + 1];
    }
    if (t_ovtab_n) ov_lo = lo, ov_span = hi - lo;
}

uint32_t t_lookup(uint32_t vram) {
    uint32_t key = vram - ov_lo < ov_span ? ovl_key(vram) : vram, fn = th_probe(key);
    if (!fn && key != vram) fn = ovl_need_key(key);  // an overlay loaded without a PI DMA, or not yet translated
    if (!fn && t_ram_code) fn = t_ram_code(vram, key);
    if (!fn) die("t_lookup: no function", vram, 0);
    return fn;
}

void t_untranslated(uint32_t vram) {
    os_die("t_untranslated", vram);  // with the threads' guest call chains, to show who jumped there
}

void t_external(struct tctx *c, uint32_t vram) {
    uint32_t fn = t_ram_code ? t_ram_code(vram, vram) : 0;
    if (!fn) die("t_external", vram, 0);
    t_call((void (*)(void))fn, c);
}

void t_badjump(void) {
    die("t_badjump", 0, 0);
}
