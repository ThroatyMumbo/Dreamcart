// See mapleraw.h. Built on the same frame/queue primitives dc/maple/vmu.c uses, with its own frame
// rather than the device's: a device KOS attached no driver to has no maple_device_t to borrow one
// from, and that is exactly the case the cart will be in.
#include <kos.h>
#include <dc/maple.h>
#include <arch/irq.h>
#include <arch/timer.h>
#include <string.h>

#include "mapleraw.h"

#define MR_TIMEOUT_MS 200

static maple_frame_t  g_frame;
static uint32_t       g_args[MR_MAX_ARGS];
static volatile int   g_done;

// Leaves the frame RESPONDED on purpose — mr_xact unlocks it once it has copied the reply out.
static void mr_cb(maple_state_t *st, maple_frame_t *frm) {
    (void)st; (void)frm;
    g_done = 1;
}

int mr_xact(int port, int unit, int cmd, const uint32_t *args, int nargs, mr_reply_t *r) {
    maple_response_t *resp;
    uint64_t t0;
    int n;

    if(nargs > MR_MAX_ARGS) nargs = MR_MAX_ARGS;
    memset(r, 0, sizeof *r);
    r->response = MAPLE_RESPONSE_NONE;

    if(maple_frame_lock(&g_frame) < 0) {
        r->rc = MR_EBUSY;
        return MR_EBUSY;
    }

    maple_frame_init(&g_frame);
    if(nargs > 0) memcpy(g_args, args, (size_t)nargs * 4);
    g_frame.cmd = cmd;
    g_frame.dst_port = port;
    g_frame.dst_unit = unit;
    g_frame.length = nargs;
    g_frame.send_buf = nargs > 0 ? g_args : NULL;
    g_frame.callback = mr_cb;

    g_done = 0;
    t0 = timer_us_gettime64();
    maple_queue_frame(&g_frame);

    while(!g_done && (timer_us_gettime64() - t0) < MR_TIMEOUT_MS * 1000ull)
        thd_sleep(1);

    r->us = (uint32_t)(timer_us_gettime64() - t0);

    if(!g_done) {
        // A device answering MAPLE_RESPONSE_AGAIN is requeued by the DMA handler forever, so the
        // frame has to come off the queue before its state is forced back.
        maple_queue_remove(&g_frame);
        g_frame.state = MAPLE_FRAME_VACANT;
        r->rc = MR_ETIMEOUT;
        return MR_ETIMEOUT;
    }

    resp = (maple_response_t *)g_frame.recv_buf;
    r->response = resp->response;
    r->dst_addr = resp->dst_addr;
    r->src_addr = resp->src_addr;
    r->data_len = resp->data_len;

    // A refusal carries a payload too — a real VMU answers a read past its last block with FILEERR
    // and one word behind it — so the length is honoured for every response but NONE. NONE is the
    // absence of a frame: nothing was received and the header is whatever the buffer already held,
    // so believing its data_len dumps a kilobyte of stale buffer into the capture.
    if(resp->response == MAPLE_RESPONSE_NONE) r->dst_addr = r->src_addr = r->data_len = 0;
    n = resp->response == MAPLE_RESPONSE_NONE ? 0 : (int)resp->data_len * 4;
    if(n > MR_MAX_DATA) n = MR_MAX_DATA;
    r->nbytes = (uint16_t)n;
    if(n > 0) memcpy(r->data, resp->data, (size_t)n);

    maple_frame_unlock(&g_frame);
    return 0;
}
