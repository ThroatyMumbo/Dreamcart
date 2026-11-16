#ifndef XL_INT_H
#define XL_INT_H

// xlate's internals that its parts share: xlate.c (meta, IR, ops, control flow, layout), xl_abi.c, xl_crc.c
#include <stdint.h>
#include <string.h>

#include "../cartscan/ovkey.h"
#include "rabbitizer.h"
#include "xlate.h"

#define BIT(k) (1ULL << (k))
#define ALL (~0ULL)
#define FOR_BITS(k, mask) for (uint64_t _w = (mask); _w && ((k) = __builtin_ctzll(_w), 1); _w &= _w - 1)
#define RANGE(a, b) ((((b) >= 63 ? 0ULL : BIT((b) + 1)) - 1) & ~(BIT(a) - 1))
static const uint64_t G_STORE = BIT(4) | BIT(5) | BIT(6) | BIT(7) | RANGE(16, 23) | BIT(28) | BIT(29) | BIT(30);
static const uint64_t G_RELOAD = BIT(2) | BIT(3) | RANGE(16, 23) | BIT(28) | BIT(29) | BIT(30);
#define G_EXIT G_RELOAD
static const uint64_t F_STORE = RANGE(12, 15) | RANGE(20, 31);
static const uint64_t F_RELOAD = RANGE(0, 3) | RANGE(20, 31);
#define F_EXIT F_RELOAD

#define SEG_MAX 64  // cartscan's CS_OV_SLOTS overlays, the boot segment and the placed ones
typedef struct { uint32_t vram, rom, size; const uint8_t *data; } Seg;
typedef struct { uint32_t jr, n; const uint8_t *targets; } JTab;

// The meta being translated (load_meta)
struct xl_meta {
    uint32_t nseg, novl;
    Seg seg[SEG_MAX];
    uint32_t nfunc;
    uint32_t *start, *end;
    uint32_t ntab;
    JTab *tab;
    uint32_t nhook;
    uint32_t *hook_addr;
    const char **hook_str;
    int *hook_name;
    uint32_t nentry;
    uint32_t *entry_addr;
    const char **entry_name;
    uint32_t nnames;
    char **names;
    int *name_und;
    uint32_t nundv;
    uint32_t *undv;
    int *undv_id;
    uint8_t *raw;  // per function: a bal target, which keeps no ABI
    uint8_t *keep;  // per function: it reads after a call registers the ABI lets the callee clobber
    int lazy;      // XLM3: overlays are translated one at a time, so a call into another overlay goes through t_lookup
    uint32_t next, *ext;  // XLM3: the functions outside this meta, sorted, and whether each takes every register
    const uint8_t *ext_raw;
};
extern struct xl_meta xl_m;
#define M xl_m

static inline uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline int is_bal(uint32_t raw) { return (raw & 0xFFFF0000u) == 0x04110000u; }
// bltzal $zero: never taken, only sets $ra to pc + 8 (a get-PC idiom)
static inline int is_getpc(uint32_t raw) { return (raw & 0xFFFF0000u) == 0x04100000u; }

// xlate.c
void load_meta(const uint8_t *b, size_t len);
void init_ops(void);
int find_u32(const uint32_t *a, uint32_t n, uint32_t v);
uint32_t word(uint32_t vram);
uint32_t ov_resolve(uint32_t from, uint32_t v, int *many);
uint32_t j_target(uint32_t vram, uint32_t raw);

// xl_abi.c: M.raw and M.keep, which functions take or keep registers outside the ABI
void raw_pass(void);
void keep_pass(void);

#endif
