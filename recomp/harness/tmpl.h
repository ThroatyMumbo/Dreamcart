#ifndef TMPL_H
#define TMPL_H

#include "htypes.h"

struct tctx {
    uint32_t fcond, hi, lo;
    uint32_t gpr[32];
    uint32_t fpr[32];
    uint32_t fcsr;  // cfc1/ctc1 $31 and cvt.w/cvt.l's rounding mode, all in t_generic; last, so no offset moves
};

#define TCTX_GPR_OFF 12
#define TCTX_FPR_OFF (12 + 32 * 4)
extern uint32_t t_rdram_bias;
// Guest memory at a KSEG0 address, in the host-order words RDRAM is kept in
#define M32(a) (*(volatile uint32_t *)((a) + t_rdram_bias))
#define M16(a) (*(volatile uint16_t *)(((a) ^ 2) + t_rdram_bias))
#define M8(a) (*(volatile uint8_t *)(((a) ^ 3) + t_rdram_bias))

// The translated code's trampoline and {vram, code} table, set by the RAM loader (tload.c)
extern void (*t_call_fn)(void (*fn)(void), struct tctx *ctx);
extern const uint32_t *t_functab_p;
extern uint32_t t_functab_n;
extern const uint32_t *t_ovtab_p;  // per overlay: run-time vram, size, ROM offset, key of that vram
extern uint32_t t_ovtab_n;
extern uint32_t (*t_ram_code)(uint32_t vram, uint32_t key);  // code the game wrote to RDRAM: translated, under key
void t_lookup_insert(uint32_t key, uint32_t fn);
void t_lookup_remove(uint32_t key);
uint32_t t_lookup_exact(uint32_t key);  // 0 when no function has that key; no overlay mapping
#define t_call(fn, ctx) t_call_fn((fn), (ctx))

void t_generic(struct tctx *c, uint32_t raw, uint32_t vram);
void t_external(struct tctx *c, uint32_t vram);
void t_badjump(void);

#endif
