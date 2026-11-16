#ifndef ALIST_DECODE_H
#define ALIST_DECODE_H

#include "htypes.h"

struct hle_t;

#define MAXC 4096u

enum { F_STATE = 1, F_KEEP = 2, F_CAP = 4, F_ENV = 8, F_EXEC = 16, F_RES = 32, F_TABR = 64, F_TABW = 128, F_RESID = 256, F_KEYON = 512,
       F_MIX = 1024 };  // F_MIX: a mixer into the dry left or right, a voice's sink when a resampler feeds it
enum { K_OTHER, K_ADPCM, K_CLEAR, K_MOVE, K_LOAD, K_BOOK };  // what provenance follows through, or stops at

struct rng { uint16_t a, n; };
struct cmd {
    uint16_t flags, wm;  // wm: the 256-byte DMEM words its writes touch
    uint8_t nr, nw, kind, af, vs;  // af: ADPCM's A_INIT, A_LOOP and 4 for 2-bit; vs: F_CAP resampler's srcs index
    union {
        struct { uint16_t in, out, cnt, feed; };  // F_RES/F_ENV: buffers and byte count; F_ENV: capture index + 1 feeding it
                                                  // F_MIX: cnt is its Q15 gain, af 1 once its other side has reported
        struct { uint32_t ram; uint16_t rlen; };  // K_LOAD: RDRAM source; K_BOOK: codebook address and bytes
    };
    struct rng r[5], w[4];  // only the first nr and nw are set
};
#define CR(c, k) (c)->r[k]
#define CW(c, k) (c)->w[k]

extern struct cmd cmds[MAXC];

// One walk of the list at RDRAM list: dies on any command that reaches outside RDRAM or DMEM, and records each
// command's DMEM ranges in cmds[]; returns the command count
uint32_t alist_decode(struct hle_t *h, int abi, uint32_t list, uint32_t size);

#endif
