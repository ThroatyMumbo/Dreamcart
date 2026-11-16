#include "tmpl.h"
#include "sim.h"

typedef unsigned long long u64;
typedef long long s64;

#define TWO63 9223372036854775808.0

static u64 arg64(struct tctx *c, int r) { return (u64)c->gpr[r] << 32 | c->gpr[r + 1]; }
static void ret64(struct tctx *c, u64 v) { c->gpr[2] = (uint32_t)(v >> 32); c->gpr[3] = (uint32_t)v; }

static double getd(struct tctx *c, int r) {
    union { double d; uint32_t w[2]; } u = { .w = { c->fpr[r], c->fpr[r + 1] } };
    return u.d;
}
static void setd(struct tctx *c, int r, double v) {
    union { double d; uint32_t w[2]; } u = { v };
    c->fpr[r] = u.w[0];
    c->fpr[r + 1] = u.w[1];
}
static float getf(struct tctx *c, int r) { union { uint32_t u; float f; } x = { c->fpr[r] }; return x.f; }
static void setf(struct tctx *c, int r, float v) { union { float f; uint32_t u; } x = { v }; c->fpr[r] = x.u; }

// R4300 ddivu/ddiv by zero: quotient all ones (signed: -1 or 1), remainder the dividend
static u64 udiv(u64 a, u64 b) { return b ? a / b : ~0ull; }
static u64 urem(u64 a, u64 b) { return b ? a % b : a; }
static s64 sdiv(s64 a, s64 b) {
    if (!b) return a < 0 ? 1 : -1;
    return b == -1 ? (s64)(0 - (u64)a) : a / b;
}
static s64 srem(s64 a, s64 b) { return !b ? a : b == -1 ? 0 : a % b; }

void h_ull_rshift(struct tctx *c) { ret64(c, arg64(c, 4) >> (arg64(c, 6) & 63)); }
void h_ll_lshift(struct tctx *c) { ret64(c, arg64(c, 4) << (arg64(c, 6) & 63)); }
void h_ll_rshift(struct tctx *c) { ret64(c, (u64)((s64)arg64(c, 4) >> (arg64(c, 6) & 63))); }
void h_ull_div(struct tctx *c) { ret64(c, udiv(arg64(c, 4), arg64(c, 6))); }
void h_ull_rem(struct tctx *c) { ret64(c, urem(arg64(c, 4), arg64(c, 6))); }
void h_ll_div(struct tctx *c) { ret64(c, (u64)sdiv((s64)arg64(c, 4), (s64)arg64(c, 6))); }
void h_ll_mul(struct tctx *c) { ret64(c, arg64(c, 4) * arg64(c, 6)); }

void h_ll_mod(struct tctx *c) {
    s64 a = (s64)arg64(c, 4), b = (s64)arg64(c, 6), t = srem(a, b);
    if ((t < 0 && b > 0) || (t > 0 && b < 0)) t += b;
    ret64(c, (u64)t);
}

void h_ull_divremi(struct tctx *c) {
    u64 a = arg64(c, 6), b = (u64)(s64)(int16_t)M16(c->gpr[29] + 18);  // libultra reads the u16 with lh
    u64 q = udiv(a, b), r = urem(a, b);
    M32(c->gpr[4]) = (uint32_t)(q >> 32);
    M32(c->gpr[4] + 4) = (uint32_t)q;
    M32(c->gpr[5]) = (uint32_t)(r >> 32);
    M32(c->gpr[5] + 4) = (uint32_t)r;
}

// The R4300 traps converting magnitudes of 2^53 and up, so only the defined range matters below.
static s64 trunc_ll(double d) { return d > -TWO63 && d < TWO63 ? (s64)d : (s64)0x8000000000000000ull; }

static u64 to_ull(double d) {
    if (d > -1.0 && d < TWO63) return (u64)(s64)d;
    if (d >= TWO63 && d - TWO63 < TWO63) return (u64)(s64)(d - TWO63) | 0x8000000000000000ull;
    return ~0ull;
}

void h_d_to_ll(struct tctx *c) { ret64(c, (u64)trunc_ll(getd(c, 12))); }
void h_f_to_ll(struct tctx *c) { ret64(c, (u64)trunc_ll(getf(c, 12))); }
void h_d_to_ull(struct tctx *c) { ret64(c, to_ull(getd(c, 12))); }
void h_f_to_ull(struct tctx *c) { ret64(c, to_ull(getf(c, 12))); }
void h_ll_to_d(struct tctx *c) { setd(c, 0, (double)(s64)arg64(c, 4)); }
void h_ll_to_f(struct tctx *c) { setf(c, 0, (float)(s64)arg64(c, 4)); }

// libultra's unsigned fix-up adds 2^32 where 2^64 was meant; the hooks keep the bug
void h_ull_to_d(struct tctx *c) {
    s64 s = (s64)arg64(c, 4);
    setd(c, 0, s < 0 ? (double)s + 4294967296.0 : (double)s);
}
void h_ull_to_f(struct tctx *c) {
    s64 s = (s64)arg64(c, 4);
    setf(c, 0, s < 0 ? (float)s + 4294967296.0f : (float)s);
}

void t_unsupported(uint32_t vram) {
    sim_puts("t_unsupported: libultra function needs the OS HLE");
    sim_hex(" ", vram);
    sim_puts("\n");
    sim_exit(96);
}
