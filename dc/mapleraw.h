// Raw Maple transactions from the Dreamcast side: one command to one (port, unit), reply bytes
// handed back exactly as the console received them. KOS only ever issues the commands its drivers
// need, so DEVINFO's sibling ALL_STATUS and every GETMINFO are unreachable through dc/maple/vmu.h.
#ifndef MAPLERAW_H
#define MAPLERAW_H

#include <stdint.h>

#define MR_MAX_DATA  1020       // the frame's own recv buffer is 1024, header included
#define MR_MAX_ARGS  255        // a Maple frame's length field; block writes carry up to 252 data words

#define MR_EBUSY     (-1)       // the shared raw frame is still in flight
#define MR_ETIMEOUT  (-2)

typedef struct {
    int      rc;                // 0 = a frame came back, MR_E* otherwise
    int8_t   response;          // MAPLE_RESPONSE_*, as the device sent it; NONE (-1) = silence
    uint8_t  dst_addr, src_addr, data_len;
    uint16_t nbytes;            // data_len * 4, clamped to MR_MAX_DATA
    uint32_t us;                // queue to reply, so it carries the wait for the next vblank
    uint8_t  data[MR_MAX_DATA];
} mr_reply_t;

// Blocking, ~200 ms cap. Safe to call for an address no device answers: rc stays 0 and response
// reads MAPLE_RESPONSE_NONE, which is how the survey tells silence from a refusal.
int mr_xact(int port, int unit, int cmd, const uint32_t *args, int nargs, mr_reply_t *r);

#endif
