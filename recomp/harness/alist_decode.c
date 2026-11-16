// Audio command lists decoded once per task: the bounds every build needs before mupen's code runs, and the DMEM
// ranges alist_aica.c's voice split works from. ABI1 (SM64), nead (MK64) and naudio (Kirby 64).
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "alist.h"
#include "hle_internal.h"
#include "memory.h"

#include "alist_aica.h"
#include "alist_decode.h"
#include "os.h"
#include "rsp_audio.h"
#include "sim.h"
#include "tmpl.h"

struct cmd cmds[MAXC];

struct abi1_st { uint32_t in, out, count, dr, wl, wr, loop; };
static struct { struct hle_t *h; uint32_t list, size, at; const struct abi1_st *abi1; } die_at;

static uint32_t al(uint32_t v, uint32_t n) { return (v + n - 1) & ~(n - 1); }

// Prints the failing command, ABI1's buffer state there, and every command of the list up to it, then dies
__attribute__((noinline, cold)) static void scan_die(const char *why, uint32_t v, uint32_t len) {
    uint32_t list = die_at.list, p = list + 8 * die_at.at;
    sim_hex("rsp audio: list", list);
    sim_hex(" size", die_at.size);
    sim_hex(" at", p);
    sim_hex(" len", len);
    sim_puts("\n");
    if (die_at.abi1) {
        const struct abi1_st *s = die_at.abi1;
        sim_hex("abi1 in", s->in);
        sim_hex(" out", s->out);
        sim_hex(" count", s->count);
        sim_hex(" dr", s->dr);
        sim_hex(" wl", s->wl);
        sim_hex(" wr", s->wr);
        sim_hex(" loop", s->loop);
        sim_puts("\n");
    }
    for (uint32_t q = list; q <= p; q += 8) {
        sim_hex(q == p ? "al*" : "al ", (q - list) / 8);
        sim_hex(" ", *dram_u32(die_at.h, q));
        sim_hex(" ", *dram_u32(die_at.h, q + 4));
        sim_puts("\n");
    }
    os_die(why, v);
}

static void need_dram(uint32_t a, uint32_t n, uint32_t cmd) {
    if (a < RDRAM_BYTES && n <= RDRAM_BYTES - a) return;
    scan_die("rsp audio: command addresses RDRAM out of range", cmd << 24 | (a & 0xFFFFFF), n);
}

static void need_dmem(uint32_t a, uint32_t n, uint32_t cmd) {
    if (a + n > 0x1000) scan_die("rsp audio: command addresses DMEM out of range", cmd << 24 | a, n);
}

// The ranges are left as they were: only the first nr and nw are read
static void open_cmd(struct cmd *c) { memset(c, 0, __builtin_offsetof(struct cmd, r)); }

__attribute__((always_inline)) static inline void rd(struct cmd *c, uint32_t a, uint32_t n) {
    a &= 0xFFF;
    if (!n || c->nr == 5) return;
    CR(c, c->nr) = (struct rng){ (uint16_t)a, (uint16_t)(a + n > 0x1000 ? 0x1000 - a : n) };
    c->nr++;
}

__attribute__((always_inline)) static inline void wr(struct cmd *c, uint32_t a, uint32_t n) {
    a &= 0xFFF;
    if (!n || c->nw == 4) return;
    n = a + n > 0x1000 ? 0x1000 - a : n;
    CW(c, c->nw) = (struct rng){ (uint16_t)a, (uint16_t)n };
    c->nw++;
    c->wm |= (uint16_t)(((2u << ((a + n - 1) >> 8)) - 1) & ~((1u << (a >> 8)) - 1));
}

static void resample_cmd(struct cmd *c, uint32_t in, uint32_t out, uint32_t cnt, uint32_t pitch) {
    c->flags |= F_RES;
    c->in = (uint16_t)in, c->out = (uint16_t)out, c->cnt = (uint16_t)cnt;
    wr(c, in - 8, 8);
    wr(c, out, cnt);
    rd(c, in - 8, 8 + 2 * ((0xFFFFu + (cnt >> 1) * pitch) >> 16));
}

// ABI1's SPNOOP, SEGMENT, SETBUFF, SETVOL and SETLOOP: the state the other commands read; 0 for any other command
static int abi1_state(struct abi1_st *s, uint32_t *seg, struct cmd *c, uint32_t op, uint32_t w1, uint32_t w2, uint32_t addr) {
    uint32_t so = (w2 >> 24) & 0x3F;
    switch (op) {
    case 0: case 9: break;
    case 7: if (so < N_SEGMENTS) seg[so] = w2 & 0xFFFFFF; break;
    case 8:
        if ((w1 >> 16) & A_AUX) {
            s->dr = (w1 + ABI1_DMEM_BASE) & 0xFFFF;
            s->wl = ((w2 >> 16) + ABI1_DMEM_BASE) & 0xFFFF;
            s->wr = (w2 + ABI1_DMEM_BASE) & 0xFFFF;
        } else {
            s->in = (w1 + ABI1_DMEM_BASE) & 0xFFFF;
            s->out = ((w2 >> 16) + ABI1_DMEM_BASE) & 0xFFFF;
            s->count = w2 & 0xFFFF;
        }
        break;
    case 15: s->loop = addr; break;
    default: return 0;
    }
    c->flags = F_STATE;
    return 1;
}

// ABI1's CLEAR, DMEMMOVE, MIXER and INTERLEAVE: DMEM to DMEM; 0 for any other command
static int abi1_dmem(const struct abi1_st *s, struct cmd *c, uint32_t op, uint32_t w1, uint32_t w2) {
    uint32_t a = (w1 + ABI1_DMEM_BASE) & 0xFFFF, b = ((w2 >> 16) + ABI1_DMEM_BASE) & 0xFFFF, o = (w2 + ABI1_DMEM_BASE) & 0xFFFF;
    switch (op) {
    case 2:
        need_dmem(((w1 & 0xFFFF) + ABI1_DMEM_BASE) & 0xFFFF, al(w2 & 0xFFF, 16), op);
        c->kind = K_CLEAR;
        if (w2 & 0xFFF) wr(c, a, al(w2 & 0xFFF, 16));
        return 1;
    case 10:  // byte moves masked to DMEM by alist.c
        c->kind = K_MOVE;
        if (w2 & 0xFFFF) rd(c, a, al(w2 & 0xFFFF, 16)), wr(c, b, al(w2 & 0xFFFF, 16));
        return 1;
    case 12:
        if (!s->count) return 1;
        need_dmem(b, al(s->count, 32), op);
        need_dmem(o, al(s->count, 32), op);
        rd(c, b, al(s->count, 32)), rd(c, o, al(s->count, 32)), wr(c, o, al(s->count, 32));
        return 1;
    case 13:
        if (!s->count) return 1;
        need_dmem(b, al(s->count, 16), op);
        need_dmem(o, al(s->count, 16), op);
        need_dmem(s->out, al(s->count, 16) * 2, op);
        rd(c, b, al(s->count, 16)), rd(c, o, al(s->count, 16)), wr(c, s->out, 2 * al(s->count, 16));
        return 1;
    }
    return 0;
}

// ABI1's ADPCM, ENVMIXER, LOADBUFF, RESAMPLE, SAVEBUFF, LOADADPCM and POLEF, each with its RDRAM operand
static int abi1_dram(struct hle_t *h, const struct abi1_st *s, struct cmd *c, uint32_t op, uint32_t w1, uint32_t addr) {
    uint32_t flags = (w1 >> 16) & 0xFF, k16 = al(s->count, 16), k8 = al(s->count, 8);
    switch (op) {
    case 1:
        need_dram(addr, 32, op);
        if (flags & A_LOOP) need_dram(s->loop, 32, op);
        c->flags = F_TABR, c->kind = K_ADPCM, c->af = (uint8_t)(flags & 3), c->ram = addr & 0xFFFFFF;
        rd(c, s->in, al(s->count, 32) / 32 * 9);
        wr(c, s->out, 32 + al(s->count, 32));
        return 1;
    case 3:  // alist_envmix_exp works in 16-byte blocks
        need_dram(addr, 80, op);
        need_dmem(s->in, k16, op);
        need_dmem(s->out, k16, op);
        need_dmem(s->dr, k16, op);
        if (flags & A_AUX) { need_dmem(s->wl, k16, op); need_dmem(s->wr, k16, op); }
        c->flags = F_ENV;
        c->in = (uint16_t)s->in, c->cnt = (uint16_t)k16;
        rd(c, s->in, k16), rd(c, s->out, k16), rd(c, s->dr, k16);
        wr(c, s->out, k16), wr(c, s->dr, k16);
        if (flags & A_AUX) rd(c, s->wl, k16), rd(c, s->wr, k16), wr(c, s->wl, k16), wr(c, s->wr, k16);
        return 1;
    case 4:
        c->kind = K_LOAD, c->ram = addr & 0xFFFFF8u;
        if (!s->count) return 1;
        need_dram(addr & ~7u, k8, op);
        need_dmem(s->in & ~3u, k8, op);
        wr(c, s->in & ~3u, k8);
        return 1;
    case 5:  // DMEM through the masked sample()
        need_dram(addr, 16, op);
        resample_cmd(c, s->in, s->out, k16, (w1 & 0xFFFF) << 1);
        return 1;
    case 6:
        if (!s->count) return 1;
        need_dram(addr & ~7u, k8, op);
        need_dmem(s->out & ~3u, k8, op);
        rd(c, s->out & ~3u, k8);
        return 1;
    case 11:
        if (al(w1 & 0xFFFF, 8) > sizeof h->alist_audio.table) os_die("rsp audio: ADPCM codebook too large", w1);
        need_dram(addr, al(w1 & 0xFFFF, 8), op);
        c->flags = F_STATE | F_TABW, c->kind = K_BOOK, c->ram = addr & 0xFFFFFF, c->rlen = (uint16_t)al(w1 & 0xFFFF, 8);
        return 1;
    case 14:
        c->flags = F_TABR | F_TABW;
        if (!s->count) return 1;
        need_dram(addr, 16, op);
        need_dmem(s->out, k16, op);
        need_dmem(s->in, k16, op);
        rd(c, s->in, k16), wr(c, s->out, k16);
        return 1;
    }
    return 0;
}

// ABI1's own segment and buffer state, so no command can reach outside RDRAM or DMEM
__attribute__((noinline)) static void decode_abi1(struct hle_t *h, uint32_t list, uint32_t n) {
    struct abi1_st s = { 0 };
    uint32_t seg[N_SEGMENTS] = { 0 };
    die_at.abi1 = &s;
    for (uint32_t i = 0; i < n; i++) {
        struct cmd *c = &cmds[i];
        uint32_t w1 = *dram_u32(h, list + 8 * i), w2 = *dram_u32(h, list + 8 * i + 4);
        uint32_t op = (w1 >> 24) & 0x7F, so = (w2 >> 24) & 0x3F;
        uint32_t addr = (so < N_SEGMENTS ? seg[so] : 0) + (w2 & 0xFFFFFF);
        die_at.at = i;
        if (so >= N_SEGMENTS && op != 2 && op != 8 && op != 9 && op != 10 && op != 12 && op != 13)
            os_die("rsp audio: segment number out of range", w2);
        open_cmd(c);
        if (!abi1_state(&s, seg, c, op, w1, w2, addr) && !abi1_dram(h, &s, c, op, w1, addr) && !abi1_dmem(&s, c, op, w1, w2))
            os_die("rsp audio: command outside ABI1", w1);
    }
    die_at.abi1 = 0;
}

// nead (MK64's ABI2): its RDRAM operands with the bytes each can touch; DMEM goes through alist.c's masked helpers
__attribute__((noinline)) static void decode_nead(struct hle_t *h, uint32_t list, uint32_t n) {
    uint32_t in = 0, out = 0, count = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct cmd *c = &cmds[i];
        uint32_t w1 = *dram_u32(h, list + 8 * i), w2 = *dram_u32(h, list + 8 * i + 4);
        uint32_t op = (w1 >> 24) & 0x7F, flags = (w1 >> 16) & 0xFF, a = w2 & 0xFFFFFF;
        die_at.at = i;
        open_cmd(c);
        if (op >= 0x20) os_die("alist_aica: nead command out of range", w1);
        switch (op) {
        case 0x01:
            need_dram(a, 0x20, op);
            c->flags = F_TABR, c->kind = K_ADPCM, c->af = (uint8_t)(flags & 7), c->ram = a;
            rd(c, in, (count + 0x1F) / 32 * (flags & 4 ? 5 : 9));
            wr(c, out, 32 + ((count + 0x1F) & ~0x1Fu));
            break;
        case 0x02: c->kind = K_CLEAR; if (w2 & 0xFFF) wr(c, w1, w2 & 0xFFF); break;
        case 0x05:
            need_dram(a, 0x20, op);
            resample_cmd(c, in, out, (count + 0xF) & ~0xFu, (w1 & 0xFFFF) << 1);
            break;
        case 0x08: c->flags = F_STATE; in = w1 & 0xFFFF; out = w2 >> 16; count = w2 & 0xFFFF; break;
        case 0x0A:
            c->kind = K_MOVE;
            if (w2 & 0xFFFF) { rd(c, w1, ((w2 & 0xFFFF) + 3) & ~3u); wr(c, w2 >> 16, ((w2 & 0xFFFF) + 3) & ~3u); }
            break;
        case 0x0B:
            need_dram(a, w1 & 0xFFFF, op);
            c->flags = F_STATE | F_TABW, c->kind = K_BOOK, c->ram = a, c->rlen = (uint16_t)(w1 & 0xFFFF);
            break;
        case 0x0C: {
            uint32_t k = (w1 >> 12) & 0xFF0;
            rd(c, w2 >> 16, k); rd(c, w2 & 0xFFFF, k); wr(c, w2 & 0xFFFF, k);
            break;
        }
        case 0x0D:
            if (count) { rd(c, w2 >> 16, count); rd(c, w2 & 0xFFFF, count); wr(c, out, 2 * count); }
            break;
        case 0x0E:
            need_dram(a, 0x20, op);
            c->flags = F_TABR | F_TABW;
            if (count) { rd(c, in, al(count, 16)); wr(c, out, al(count, 16)); }
            break;
        case 0x0F: need_dram(a, 0x20, op); c->flags = F_STATE; break;  // SETLOOP
        case 0x10: {
            uint32_t blocks = (w1 >> 16) & 0xFF, bs = al(w2 & 0xFFFF, 32), k = (blocks ? blocks : 1) * (bs ? bs : 32);
            rd(c, w1 & 0xFFFF, k); wr(c, w2 >> 16, k);
            break;
        }
        case 0x11: rd(c, w2 >> 16, 4 * (w1 & 0xFFFF)); wr(c, w2 & 0xFFFF, 2 * (w1 & 0xFFFF)); break;
        case 0x13: {
            uint32_t k = 2 * al((w1 >> 8) & 0xFF, 8), d[4] = { (w2 >> 20) & 0xFF0, (w2 >> 12) & 0xFF0, (w2 >> 4) & 0xFF0, (w2 << 4) & 0xFF0 };
            c->flags = F_ENV;
            c->in = (uint16_t)((w1 >> 12) & 0xFF0), c->cnt = (uint16_t)k;
            rd(c, c->in, k);
            for (int j = 0; j < 4; j++) { rd(c, d[j], k); wr(c, d[j], k); }
            break;
        }
        case 0x14:
            need_dram(a, (w1 >> 12) & 0xFFF, op);
            c->kind = K_LOAD, c->ram = w2 & 0xFFFFF8u;
            wr(c, w1 & 0xFFC, al((w1 >> 12) & 0xFFF, 8));
            break;
        case 0x15: need_dram(a, (w1 >> 12) & 0xFFF, op); rd(c, w1 & 0xFFC, al((w1 >> 12) & 0xFFF, 8)); break;
        default: c->flags = F_STATE; break;  // SPNOOP, SEGMENT, SETBUFF state, ENVSETUP1/2
        }
    }
}

// naudio: RDRAM as nead, plus the MAIN-relative DMEM operands that alist.c's load/save/mix use unmasked
__attribute__((noinline)) static void decode_naudio(struct hle_t *h, uint32_t list, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        struct cmd *c = &cmds[i];
        uint32_t w1 = *dram_u32(h, list + 8 * i), w2 = *dram_u32(h, list + 8 * i + 4);
        uint32_t op = (w1 >> 24) & 0x7F, a1 = w1 & 0xFFFFFF, a2 = w2 & 0xFFFFFF;
        uint32_t k = al((w1 >> 12) & 0xFFF, 8), d = ((w1 & 0xFFF) + NA_MAIN) & ~3u;
        die_at.at = i;
        open_cmd(c);
        switch (op) {
        case 0x00: case 0x09: case 0x0E: c->flags = F_STATE; break;  // SPNOOP, SETVOL, the 0x12B0 rate patch
        case 0x01: {
            uint32_t b = (((w2 >> 16) & 0xFFF) + 0x1F) & ~0x1Fu;
            need_dram(a1, 32, op);
            c->flags = F_TABR, c->kind = K_ADPCM, c->af = (uint8_t)((w2 >> 28) & 3), c->ram = a1;
            rd(c, ((w2 >> 12) & 0xF) + NA_MAIN, b / 32 * 9);
            wr(c, (w2 & 0xFFF) + NA_MAIN, 32 + b);
            break;
        }
        case 0x02: c->kind = K_CLEAR; wr(c, (uint16_t)(w1 + NA_MAIN), w2 & 0xFFF); break;
        case 0x03:
            need_dram(a2, 80, op);
            c->flags = F_ENV;
            c->in = NA_MAIN, c->cnt = NA_COUNT;
            rd(c, NA_MAIN, NA_COUNT);
            rd(c, NA_DL, NA_COUNT); rd(c, NA_DR, NA_COUNT); rd(c, NA_WL, NA_COUNT); rd(c, NA_WR, NA_COUNT);
            wr(c, NA_DL, NA_COUNT); wr(c, NA_DR, NA_COUNT); wr(c, NA_WL, NA_COUNT); wr(c, NA_WR, NA_COUNT);
            break;
        case 0x04:
            need_dram(a2 & ~7u, k, op);
            need_dmem(d, k, op);
            c->kind = K_LOAD, c->ram = w2 & 0xFFFFF8u;
            wr(c, d, k);
            break;
        case 0x05:
            need_dram(a1, 0x20, op);
            resample_cmd(c, ((w2 >> 2) & 0xFFF) + NA_MAIN, w2 & 3 ? NA_MAIN2 : NA_MAIN, NA_COUNT, ((w2 >> 14) & 0xFFFF) << 1);
            break;
        case 0x06: need_dram(a2 & ~7u, k, op); need_dmem(d, k, op); rd(c, d, k); break;
        case 0x0A: {
            uint32_t m = (uint16_t)(((w2 & 0xFFFF) + 3) & ~3u);
            c->kind = K_MOVE;
            rd(c, (uint16_t)(w1 + NA_MAIN), m); wr(c, (uint16_t)((w2 >> 16) + NA_MAIN), m);
            break;
        }
        case 0x0B:
            if ((w1 & 0xFFFF) > sizeof h->alist_naudio.table) os_die("rsp audio: ADPCM codebook too large", w1);
            need_dram(a2, w1 & 0xFFFF, op);
            c->flags = F_STATE | F_TABW, c->kind = K_BOOK, c->ram = a2, c->rlen = (uint16_t)(w1 & 0xFFFF);
            break;
        case 0x0C: {
            uint32_t s = (uint16_t)((w2 >> 16) + NA_MAIN), o = (uint16_t)(w2 + NA_MAIN);
            need_dmem(s, NA_COUNT, op);
            need_dmem(o, NA_COUNT, op);
            rd(c, s, NA_COUNT); rd(c, o, NA_COUNT); wr(c, o, NA_COUNT);
            if (o == NA_DL || o == NA_DR) c->flags = F_MIX, c->in = (uint16_t)s, c->out = (uint16_t)o, c->cnt = (uint16_t)w1;
            break;
        }
        case 0x0D: rd(c, NA_DL, NA_COUNT); rd(c, NA_DR, NA_COUNT); wr(c, NA_MAIN, 2 * NA_COUNT); break;
        case 0x0F: need_dram(a2, 32, op); c->flags = F_STATE; break;  // SETLOOP
        default: scan_die("rsp audio: command outside naudio", w1, 0);
        }
    }
}

uint32_t alist_decode(struct hle_t *h, int abi, uint32_t list, uint32_t size) {
    uint32_t n = size / 8;
    if (list >= RDRAM_BYTES || size > RDRAM_BYTES - list) os_die("rsp audio: command addresses RDRAM out of range", 0x80u << 24 | list);
    if (n > MAXC) os_die("alist_aica: command list longer than MAXC", n);
    die_at.h = h, die_at.list = list, die_at.size = size, die_at.at = 0;
    if (abi == AA_NEAD) decode_nead(h, list, n);
    else if (abi == AA_NAUDIO) decode_naudio(h, list, n);
    else decode_abi1(h, list, n);
    return n;
}
