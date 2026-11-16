// The display-list walk both GBI families share: fetch, the call stack and the task's end. Always inlined into
// each task's loop, so its switch stays a direct one.
#ifndef GFX_DL_H
#define GFX_DL_H

#include "gfx.h"
#include "rsp_gfx.h"
#include "os.h"

#define DL_DEPTH_MAX 18

struct dl {
    const uint8_t *ram;  // gfx_rd32's base, out of the loop
    uint32_t pc, cmds, rdp, hash;
    int depth;
};

static uint32_t dl_stack[DL_DEPTH_MAX];

#define DL_INLINE static inline __attribute__((always_inline))

DL_INLINE void dl_start(struct dl *d, uint32_t pc, uint32_t hash) {
    d->ram = (const uint8_t *)(0x80000000u + t_rdram_bias);
    d->pc = pc, d->cmds = 0, d->rdp = 0, d->hash = hash, d->depth = 0;
}

// The next command: polls every 32 and folds both words into the hash; returns the opcode byte
DL_INLINE uint32_t dl_next(struct dl *d, uint32_t *w0, uint32_t *w1) {
    uint32_t pc = d->pc;
    if (pc > RDRAM_SIZE - 8) os_die("rsp gfx: display list runs out of RDRAM", pc);
    uint32_t a = *(const uint32_t *)(d->ram + pc), b = *(const uint32_t *)(d->ram + pc + 4);
    __asm__("" : "+r"(a), "+r"(b));  // or GCC re-reads them narrowed and byte-swapped for a few arms, every command
    if (!(pc & 31)) __builtin_prefetch(d->ram + pc + 64);  // the display list streams: two lines ahead
    d->pc = pc + 8;
    if (!(++d->cmds & 31) && rsp_gfx_poll) rsp_gfx_poll();
    d->hash = ((d->hash * 33) ^ a) * 33 ^ b;
    *w0 = a, *w1 = b;
    return a >> 24;
}

// G_DL: push the return address unless it's a branch; 0 when the stack already holds limit of them
DL_INLINE int dl_call(struct dl *d, struct gfx_stats *st, uint32_t to, int push, int limit) {
    st->dls++;
    if (push) {
        if (d->depth == limit) return 0;
        dl_stack[d->depth++] = d->pc;
    }
    d->pc = to;
    return 1;
}

// G_ENDDL: back to the caller, or 1 when the task's own list has ended
DL_INLINE int dl_return(struct dl *d, struct gfx_stats *st) {
    if (d->depth) {
        d->pc = dl_stack[--d->depth];
        return 0;
    }
    st->cmds = d->cmds, st->rdp = d->rdp, st->hash = d->hash;
    gfx_tmem_flush();  // the next task may find RDRAM changed under a queued load
    gfx_frame_end();
    return 1;
}

#endif
