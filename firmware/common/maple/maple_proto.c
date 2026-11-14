#include "maple_proto.h"
#include <string.h>

#define DEVINFO_LEN 112
#define ALLINFO_LEN 192

// FuncData[0] for the HKT-7700, verbatim from MaplePad's maple.c: which buttons and axes exist.
#define FUNCDATA0_HKT7700 0x000f06feu

// Verified byte for byte against a real HKT-7700 off the bus.
static const maple_devid_t k_hkt7700 = {
    .func          = MAPLE_FUNC_CONTROLLER,
    .func_data     = { FUNCDATA0_HKT7700, 0, 0 },
    .area_code     = 0xff,
    .connector_dir = 0x00,
    .name          = "Dreamcast Controller",
    .license       = "Produced By or Under License From SEGA ENTERPRISES,LTD.",
    .free_status   = "Version 1.010,1998/09/28,315-6211-AB   ,Analog Module : The 4th Edition.5/8  +DF",
    .standby_power = 430,
    .max_power     = 500,
};

static void put_u16(uint8_t *p, uint16_t v) {       // native, not swapped — as the references leave it
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

// Space padded, never NUL terminated: the padding is part of the wire format.
static void put_text(uint8_t *p, unsigned n, const char *s) {
    unsigned i = 0;
    while (i < n && s[i]) { p[i] = (uint8_t)s[i]; i++; }
    while (i < n) { p[i++] = ' '; }
}

uint8_t maple_checksum(const uint8_t *frame, unsigned n) {
    uint8_t x = 0;
    for (unsigned i = 0; i < n; i++) { x ^= frame[i]; }
    return x;
}

unsigned maple_pack_words(const uint8_t *frame, unsigned n, uint32_t *words) {
    unsigned k = n / 4U;
    for (unsigned w = 0; w < k; w++) {
        const uint8_t *p = frame + 4U * w; // little-endian load; the PIO reverses it again
        words[w] = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                 | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    // The SM shifts MSB first and the bit-pair count cuts the last word short, so the checksum
    // goes in its top byte.
    if (n & 3U) {
        words[k] = (uint32_t)frame[n - 1U] << 24;
        return k + 1U;
    }
    return k;
}

unsigned maple_unpack(const uint8_t *wire, unsigned n, uint8_t *out) {
    if (n < 5U || (n & 3U) != 1U || n > MAPLE_MAX_FRAME) { return 0; }
    unsigned k = n / 4U;
    for (unsigned w = 0; w < k; w++) {          // each whole word arrives reversed
        out[4U * w + 0] = wire[4U * w + 3];
        out[4U * w + 1] = wire[4U * w + 2];
        out[4U * w + 2] = wire[4U * w + 1];
        out[4U * w + 3] = wire[4U * w + 0];
    }
    return k * 4U; // the checksum is dropped, already verified
}

void maple_cond_default(maple_cond_t *c) {
    c->buttons = 0xffffU;
    c->trig_r = c->trig_l = 0;
    c->joy_x = c->joy_y = c->joy_x2 = c->joy_y2 = MAPLE_AXIS_CENTER;
}

static unsigned build_header(uint8_t *out, uint8_t cmd, uint8_t src, uint8_t port, unsigned nwords) {
    out[0] = cmd;
    out[1] = (uint8_t)(MAPLE_ADDR_DC | port);
    out[2] = (uint8_t)(src | port);
    out[3] = (uint8_t)nwords;
    return 4;
}

static unsigned build_devinfo(uint8_t *p, bool all, const maple_devid_t *id) {
    memset(p, 0, all ? ALLINFO_LEN : DEVINFO_LEN);
    maple_put_word(p +  0, id->func);
    maple_put_word(p +  4, id->func_data[0]);
    maple_put_word(p +  8, id->func_data[1]);
    maple_put_word(p + 12, id->func_data[2]);
    p[16] = id->area_code;
    p[17] = id->connector_dir;
    put_text(p + 18, 30, id->name);
    put_text(p + 48, 60, id->license);
    put_u16(p + 108, id->standby_power);
    put_u16(p + 110, id->max_power);
    if (all) { put_text(p + 112, 80, id->free_status); }
    return all ? ALLINFO_LEN : DEVINFO_LEN;
}

static unsigned build_cond(uint8_t *p, const maple_cond_t *c) {
    maple_put_word(p + 0, MAPLE_FUNC_CONTROLLER);
    p[4]  = (uint8_t)c->buttons;
    p[5]  = (uint8_t)(c->buttons >> 8);
    p[6]  = c->trig_r;
    p[7]  = c->trig_l;
    p[8]  = c->joy_x;
    p[9]  = c->joy_y;
    p[10] = c->joy_x2;
    p[11] = c->joy_y2;
    return 12;
}

bool maple_common_command(const maple_unit_t *u, const uint8_t *req, uint8_t *payload,
                          uint8_t *resp, int *len) {
    switch (maple_cmd(req)) {
    case MAPLE_CMD_RESET_DEVICE:
        *resp = MAPLE_CMD_RESP_ACK;
        *len = 0;
        return true;
    case MAPLE_CMD_DEVICE_REQUEST:
        *resp = MAPLE_CMD_RESP_DEVICE_STATUS;
        *len = (int)build_devinfo(payload, false, u->id);
        return true;
    case MAPLE_CMD_ALL_STATUS_REQUEST:
        *resp = MAPLE_CMD_RESP_ALL_STATUS;
        *len = (int)build_devinfo(payload, true, u->id);
        return true;
    default:
        return false;
    }
}

static int controller_command(const maple_unit_t *u, const uint8_t *req, unsigned req_len,
                              uint8_t *payload, uint8_t *resp) {
    int len;
    if (maple_common_command(u, req, payload, resp, &len)) { return len; }

    switch (maple_cmd(req)) {
    case MAPLE_CMD_GET_CONDITION:
        if (!maple_has_words(req, req_len, 1)) { return MAPLE_REPLY_NONE; }
        // A function we do not have is refused, not ignored, as a real HKT-7700 does; silence
        // here would be a device no console has ever seen.
        if (maple_data_word(req, 0) != u->id->func) {
            *resp = MAPLE_RESP_UNKNOWN_FUNC;
            return 0;
        }
        *resp = MAPLE_CMD_RESP_DATA_XFER;
        return (int)build_cond(payload, (const maple_cond_t *)u->state);

    default:
        // A real controller answers BADCMD to everything else, GET_MEDIA_INFO for its own function
        // included: command-unknown is decided before function-unknown.
        *resp = MAPLE_RESP_UNKNOWN_CMD;
        return 0;
    }
}

void maple_controller_unit(maple_unit_t *u, maple_cond_t *cond) {
    u->addr    = MAPLE_ADDR_CONTROLLER;
    u->id      = &k_hkt7700;
    u->command = controller_command;
    u->state   = cond;
}

void maple_bus_init(maple_bus_t *b) { memset(b, 0, sizeof *b); }

int maple_bus_find(const maple_bus_t *b, uint8_t dst) {
    uint8_t a = dst & MAPLE_PERIPH_MASK;
    for (unsigned i = 0; i < b->n; i++) {
        if (b->unit[i]->addr == a) { return (int)i; }
    }
    return -1;
}

bool maple_bus_add(maple_bus_t *b, const maple_unit_t *u) {
    if (b->n == MAPLE_MAX_UNITS || maple_bus_find(b, u->addr) >= 0) { return false; }
    b->unit[b->n++] = u;
    if (u->addr != MAPLE_ADDR_CONTROLLER) { b->sub_mask |= u->addr & MAPLE_SUB_MASK; }
    return true;
}

unsigned maple_build_reply(const maple_bus_t *b, const uint8_t *req, unsigned req_len,
                           uint8_t out[MAPLE_MAX_FRAME]) {
    const maple_unit_t *u;
    uint8_t port;
    uint8_t src;
    uint8_t resp = 0;
    int payload;
    int ui;
    unsigned n;

    if (req_len < 4) { return 0; }
    ui = maple_bus_find(b, maple_dst(req));
    if (ui < 0) { return 0; }
    u = b->unit[ui];

    payload = u->command(u, req, req_len, out + 4, &resp);
    if (payload < 0 || (payload & 3)) {
        return 0; // a payload is always whole words
    }

    // The echoed port bits land in both address bytes and cancel in the XOR, so a precomputed
    // checksum survives any port; the sub-device bits do not, so they go in before the sum.
    port = maple_src(req) & MAPLE_PORT_MASK;
    src  = u->addr;
    if (src == MAPLE_ADDR_CONTROLLER) { src |= b->sub_mask; }

    n = build_header(out, resp, src, port, (unsigned)payload / 4U);
    n += (unsigned)payload;
    out[n] = maple_checksum(out, n);
    return n + 1U;
}

// Bus states as the sampler reports them: bit0 = SDCKA, bit1 = SDCKB.
enum { S00 = 0, S01 = 1, S10 = 2, S11 = 3 };

// status byte: 0xff none, 0 start, 1 end, 2..9 data bit 0..7, |0x80 the bit is a one.
#define ST_NONE   0xffu
#define ST_START  0u
#define ST_END    1u
#define ST_PUSH0  2u
#define ST_PUSH7  (ST_PUSH0 + 7u)
#define ST_BITSET 0x80u

static uint8_t dfa_new(maple_rx_t *d, unsigned expect) {
    uint8_t s = d->nstates++;
    memset(d->next[s], 0xff, 4);
    d->status[s] = ST_NONE;
    d->next[s][expect] = s;                 // the sampler can repeat a level; staying is correct
    return s;
}

static uint8_t dfa_expect(maple_rx_t *d, uint8_t parent, unsigned expect) {
    uint8_t s = dfa_new(d, expect);
    d->next[parent][expect] = s;
    return s;
}

static uint8_t dfa_expect_st(maple_rx_t *d, uint8_t parent, unsigned expect, uint8_t st) {
    uint8_t s = dfa_expect(d, parent, expect);
    d->status[s] = st;
    return s;
}

static uint8_t dfa_expect2(maple_rx_t *d, uint8_t p1, uint8_t p2, unsigned expect) {
    uint8_t s = dfa_new(d, expect);
    d->next[p1][expect] = s;
    d->next[p2][expect] = s;
    return s;
}

void maple_rx_reset(maple_rx_t *r) {
    r->state = 0;
    r->byte = 0;
    r->xorsum = 0;
    r->framing = false;
    r->len = 0;
}

void maple_rx_init(maple_rx_t *r) {
    r->nstates = 0;

    // Start: SDCKA drops and stays low while SDCKB pulses four times, then both go high.
    uint8_t prev = dfa_new(r, S11);
    for (int i = 0; i < 4; i++) {
        prev = dfa_expect(r, prev, S10);
        prev = dfa_expect(r, prev, S00);
    }
    prev = dfa_expect(r, prev, S10);
    prev = dfa_expect_st(r, prev, S11, ST_START);

    // Data: one line is high between bits and acts as the clock. Going to 0b11 is a one, going to
    // 0b00 is a zero, and the roles then swap. Eight bits, MSB first, per byte.
    uint8_t option = prev;
    uint8_t possible_end = 0;
    uint8_t start_byte = r->nstates;
    for (int i = 0; i < 4; i++) {
        prev   = dfa_expect2(r, option, prev, S01);
        option = dfa_expect_st(r, prev, S11, (uint8_t)(ST_PUSH0 + i * 2) | ST_BITSET);
        prev   = dfa_expect_st(r, prev, S00, (uint8_t)(ST_PUSH0 + i * 2));
        if (i == 0) { possible_end = option; }

        prev   = dfa_expect2(r, option, prev, S10);
        option = dfa_expect_st(r, prev, S11, (uint8_t)(ST_PUSH0 + i * 2 + 1) | ST_BITSET);
        prev   = dfa_expect_st(r, prev, S00, (uint8_t)(ST_PUSH0 + i * 2 + 1));
        if (i == 3) {
            r->next[option][S01] = start_byte;
            r->next[prev][S01]   = start_byte;
        }
    }

    // End: SDCKB stays low while SDCKA pulses. The event fires two states early on purpose — the
    // data is all in by then, and waiting for the last edge would leave no time to reply.
    prev = dfa_expect(r, possible_end, S01);
    prev = dfa_expect(r, prev, S00);
    prev = dfa_expect_st(r, prev, S01, ST_END);
    prev = dfa_expect(r, prev, S00);
    prev = dfa_expect(r, prev, S01);
    r->next[prev][S11] = 0;

    maple_rx_reset(r);
}

maple_rx_ev_t maple_rx_feed(maple_rx_t *r, uint8_t sample) {
    uint8_t prev = r->state;
    uint8_t ns = r->next[prev][sample & 3U];
    if (ns == 0xffU) {
        maple_rx_reset(r);
        return MAPLE_RX_ERROR;
    }

    r->state = ns;
    if (ns == prev) {
        return MAPLE_RX_MORE; // no transition, so no data
    }

    uint8_t st = r->status[ns];
    if (st == ST_NONE) { return MAPLE_RX_MORE; }

    if (st & ST_BITSET) { r->byte |= (uint8_t)(1U << (7U - ((st & 0x7fU) - ST_PUSH0))); }

    switch (st & 0x7fU) {
    case ST_START:
        r->len = 0; r->byte = 0; r->xorsum = 0; r->framing = true;
        break;

    case ST_END: {
        if (!r->framing) { return MAPLE_RX_MORE; }
        r->framing = false;
        // A frame is whole words plus the checksum byte, and every byte including it XORs to 0.
        bool ok = r->xorsum == 0 && r->len >= 5U && (r->len & 3U) == 1U;
        return ok ? MAPLE_RX_FRAME : MAPLE_RX_ERROR;
    }

    case ST_PUSH7:
        if (!r->framing) { break; }
        if (r->len >= MAPLE_MAX_FRAME) { maple_rx_reset(r); return MAPLE_RX_ERROR; }
        r->buf[r->len++] = r->byte;
        r->xorsum ^= r->byte;
        r->byte = 0;
        break;

    default:
        break;
    }
    return MAPLE_RX_MORE;
}
