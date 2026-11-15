#include "mapledev.h"
#include "mapledev.pio.h"
#include "devcommon.h"
#include "hardware/dma.h"
#include "hardware/timer.h"
#include "hardware/gpio.h"

#define MAPLE_IRQ 7

// Console-referenced turnaround: both lines released, then a settle.
#define MAPLEDEV_IDLE_TIMEOUT_US 30
#define MAPLEDEV_TURNAROUND_US    1

// How long a frame already in progress may run past the caller's idle budget: the longest frame the
// receiver takes, at 5.184 us a byte, with half again as headroom (8 ms).
#define MAPLEDEV_FRAME_MAX_US  (MAPLE_MAX_FRAME * 5184u / 1000u * 3u / 2u)

// One PIO block is 32 words and Maple owns all of pio2. Break the build here rather than
// discovering it as a failed init on a console.
_Static_assert(PIO_WORDS(maple_tx) + PIO_WORDS(maple_rx_edge) + PIO_WORDS(maple_rx_sample) <= 32,
               "mapledev outgrew one PIO block");

static pio_res_t g_res;
static PIO      g_pio;
static uint     g_sm_tx, g_sm_sample, g_sm_edge_a, g_sm_edge_b;
static uint     g_off_tx, g_off_edge, g_off_sample;

static void tx_patch_check(void);
static uint     g_pin_a, g_pin_b;
static int      g_dma = -1;
static maple_rx_t g_rx;
static bool       g_locked;                 // a frame has decoded since the last re-arm
static mapledev_stats_t g_st;
static uint32_t   g_end_us;                 // timerawl at the decoder's end event

static void tx_park(void);

static void __not_in_flash_func(rx_set)(bool on) {
    uint32_t mask = (1U << g_sm_sample) | (1U << g_sm_edge_a) | (1U << g_sm_edge_b);
    pio_set_sm_mask_enabled(g_pio, mask, on);
}

bool mapledev_init(PIO pio, uint pin_a, uint pin_b) {
    static const struct pio_program *const progs[] = {
        &maple_tx_program, &maple_rx_edge_program, &maple_rx_sample_program,
    };

    g_pio = NULL;
    g_pin_a = pin_a;
    g_pin_b = pin_b;
    if (!pio_res_claim(&g_res, pio, progs, 3, 4)) { return false; }
    g_dma = dma_claim_unused_channel(false);
    if (g_dma < 0) { pio_res_release(&g_res); return false; }

    g_pio = pio;
    g_off_tx = g_res.off[0]; g_off_edge = g_res.off[1]; g_off_sample = g_res.off[2];
    g_sm_tx = g_res.sm[0]; g_sm_edge_a = g_res.sm[1]; g_sm_edge_b = g_res.sm[2]; g_sm_sample = g_res.sm[3];

    maple_pin_init(pio, pin_a, pin_b);
    maple_tx_program_init(pio, g_sm_tx, g_off_tx, pin_a, pin_b);
    for (unsigned r = 0; r < MAPLEDEV_TX_RATES; r++) { mapledev_set_tx_hz(r, mapledev_tx_hz(r)); }
    tx_patch_check();
    maple_rx_edge_program_init(pio, g_sm_edge_a, g_off_edge, pin_a);
    maple_rx_edge_program_init(pio, g_sm_edge_b, g_off_edge, pin_b);
    maple_rx_sample_program_init(pio, g_sm_sample, g_off_sample, pin_a);

    dma_channel_config c = dma_channel_get_default_config((uint)g_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, g_sm_tx, true));
    dma_channel_configure((uint)g_dma, &c, &pio->txf[g_sm_tx], NULL, 0, false);

    maple_rx_init(&g_rx);
    tx_park();                                  // idle is a stopped SM with the bus released
    mapledev_resync();
    return true;
}

void __not_in_flash_func(mapledev_stage)(mapledev_pkt_t *pkt, const uint8_t *frame, unsigned n) {
    pkt->w[0] = 3;                              // sync pulses; a data word, not a `set y`
    pkt->w[1] = maple_bit_pairs_minus1(n);
    pkt->nw = maple_pack_words(frame, n, &pkt->w[2]) + 2U;
    pkt->nbytes = n;
}

void __not_in_flash_func(mapledev_set_port)(mapledev_pkt_t *pkt, uint8_t port) {
    // The frame word is packed little-endian as cmd | dst<<8 | src<<16 | nwords<<24.
    uint32_t w = pkt->w[2];
    uint8_t dst = (uint8_t)((w >> 8) & MAPLE_PERIPH_MASK) | port;
    uint8_t src = (uint8_t)((w >> 16) & MAPLE_PERIPH_MASK) | port;
    pkt->w[2] = (w & 0xff0000ffU) | ((uint32_t)dst << 8) | ((uint32_t)src << 16);
}

void __not_in_flash_func(mapledev_resync)(void) {
    rx_set(false);
    pio_sm_clear_fifos(g_pio, g_sm_sample);
    pio_sm_restart(g_pio, g_sm_edge_a);
    pio_sm_restart(g_pio, g_sm_edge_b);
    pio_sm_restart(g_pio, g_sm_sample);
    // pio_sm_restart() leaves the PC alone, so re-enter explicitly.
    pio_sm_exec(g_pio, g_sm_edge_a, pio_encode_jmp(g_off_edge));
    pio_sm_exec(g_pio, g_sm_edge_b, pio_encode_jmp(g_off_edge));
    pio_sm_exec(g_pio, g_sm_sample, pio_encode_jmp(g_off_sample));
    pio_interrupt_clear(g_pio, MAPLE_IRQ);
    maple_rx_reset(&g_rx);
    g_locked = false;
    rx_set(true);
    g_st.resyncs++;
}

bool __not_in_flash_func(mapledev_poll)(uint8_t *frame, unsigned *len, uint32_t timeout_us) {
    uint32_t t0 = timer_hw->timerawl;
    uint32_t stallbit = 1U << (PIO_FDEBUG_RXSTALL_LSB + g_sm_sample);

    if (g_pio->fdebug & stallbit) { g_pio->fdebug = stallbit; g_st.rx_stall++; }
    for (;;) {
        while (!pio_sm_is_rx_fifo_empty(g_pio, g_sm_sample)) {
            uint8_t v = (uint8_t)pio_sm_get(g_pio, g_sm_sample);
            if (g_pio->fdebug & stallbit) { g_pio->fdebug = stallbit; g_st.rx_stall_drain++; }
            for (int s = 3; s >= 0; s--) {              // oldest sample sits in the high bits
                maple_rx_ev_t ev = maple_rx_feed(&g_rx, (uint8_t)((v >> (s * 2)) & 3U));
                if (ev == MAPLE_RX_ERROR) {
                    if (g_locked) {
                        g_st.bad_ev++;
                    } else {
                        g_st.desync++;
                    }
                    continue;
                }
                if (ev != MAPLE_RX_FRAME) { continue; }

                unsigned n = maple_unpack(g_rx.buf, g_rx.len, frame);
                if (n == 0) { g_st.bad_frame++; continue; }
                *len = n;
                g_locked = true;                        // synchronized: errors now mean corruption
                g_st.frames++;
                g_end_us = timer_hw->timerawl;
                return true;
            }
        }
        // Past the idle budget, finish a frame in flight (up to the cap) rather than return a partial decode.
        uint32_t waited = timer_hw->timerawl - t0;
        if (waited > timeout_us) {
            if (!maple_rx_in_frame(&g_rx)) { return false; }
            if (waited > timeout_us + MAPLEDEV_FRAME_MAX_US) {
                g_st.frame_timeout++;
                mapledev_resync();
                return false;
            }
        }
    }
}

// gpio_get, not a masked sio_hw->gpio_in read: the controller port is GP32-39 and those live in
// gpio_hi_in, so `1u << pin` on the low register is a different pin entirely.
static bool __not_in_flash_func(wait_bus_idle)(void) {
    uint32_t t0 = timer_hw->timerawl;
    while (!(gpio_get(g_pin_a) && gpio_get(g_pin_b))) {
        if (timer_hw->timerawl - t0 > MAPLEDEV_IDLE_TIMEOUT_US) { return false; }
    }

    uint32_t t1 = timer_hw->timerawl;
    while (timer_hw->timerawl - t1 < MAPLEDEV_TURNAROUND_US) { }
    return true;
}

bool __not_in_flash_func(mapledev_send)(const mapledev_pkt_t *pkt) {
    rx_set(false);                              // or we decode our own frame and overrun the FIFO

    // The last frame left the OSR partial; restarting clears it. Rearming drives nothing: the SM releases the bus and stalls on `out y,32`.
    pio_sm_set_enabled(g_pio, g_sm_tx, false);
    pio_sm_clear_fifos(g_pio, g_sm_tx);
    pio_sm_restart(g_pio, g_sm_tx);
    pio_sm_exec(g_pio, g_sm_tx, pio_encode_jmp(g_off_tx));
    pio_sm_set_enabled(g_pio, g_sm_tx, true);

    // Never drive into the console. Losing a reply costs one poll; a contention does not.
    if (!wait_bus_idle()) { g_st.no_idle++; return false; }

    dma_channel_set_read_addr((uint)g_dma, pkt->w, false);
    dma_channel_set_trans_count((uint)g_dma, pkt->nw, true);
    uint32_t turn = timer_hw->timerawl - g_end_us;
    g_st.turn_last = turn;
    if (turn > g_st.turn_max) { g_st.turn_max = turn; }
    if (!g_st.turn_min || turn < g_st.turn_min) { g_st.turn_min = turn; }
    g_st.replies++;
    return true;
}

// Restart clears the partial OSR, `set pindirs,0` releases the lines, and the SM stays disabled so it
// cannot wrap back into a frame.
static void __not_in_flash_func(tx_park)(void) {
    pio_sm_set_enabled(g_pio, g_sm_tx, false);
    pio_sm_clear_fifos(g_pio, g_sm_tx);
    pio_sm_restart(g_pio, g_sm_tx);
    pio_sm_exec(g_pio, g_sm_tx, pio_encode_set(pio_pindirs, 0));
    pio_sm_exec(g_pio, g_sm_tx, pio_encode_jmp(g_off_tx));
}

// The frame is out once the PC is back in the preamble; a fixed delay lets the SM re-drive the bus
// past `.wrap`.
static inline bool __not_in_flash_func(tx_wrapped)(void) {
    uint8_t pc = pio_sm_get_pc(g_pio, g_sm_tx);
    return (uint8_t)(pc - g_off_tx) <= 7U;
}

bool __not_in_flash_func(mapledev_wait_sent)(const mapledev_pkt_t *pkt) {
    // ~5.2 us a byte at the Maple bit rate, plus the start and end patterns.
    uint32_t budget = pkt->nbytes * 8U + 400U;
    uint32_t t0 = timer_hw->timerawl;

    while (dma_channel_is_busy((uint)g_dma)) {
        if (timer_hw->timerawl - t0 > budget) { goto late; }
    }
    while (!pio_sm_is_tx_fifo_empty(g_pio, g_sm_tx)) {
        if (timer_hw->timerawl - t0 > budget) { goto late; }
    }

    while (!tx_wrapped()) {
        if (timer_hw->timerawl - t0 > budget) { goto late; }
    }
    tx_park();
    uint32_t tx = timer_hw->timerawl - t0;
    if (tx > g_st.tx_max) { g_st.tx_max = tx; g_st.tx_bytes_max = pkt->nbytes; }
    return true;
late:
    g_st.overruns++;                            // a nonzero count is the evidence, not a fix
    tx_park();                                  // never leave the bus driven, however we got here
    return false;
}

const mapledev_stats_t *mapledev_stats(void) { return &g_st; }

static uint32_t g_tx_hz[MAPLEDEV_TX_RATES] = { maple_tx_CLK_HZ, maple_tx_CLK_HZ };
static uint32_t g_tx_clkdiv[MAPLEDEV_TX_RATES];     // raw SM CLKDIV values, so selecting is one store

// Only the TX SM: the RX side is edge-triggered and keeps its own clock. Capped at divider 1.
void mapledev_set_tx_hz(unsigned rate, uint32_t hz) {
    if (rate >= MAPLEDEV_TX_RATES || !hz) { return; }
    uint32_t sys = clock_get_hz(clk_sys);
    if (hz > sys) { hz = sys; }
    uint32_t div_int;
    uint8_t div_frac8;
    pio_calculate_clkdiv8_from_float((float)sys / (float)hz, &div_int, &div_frac8);
    g_tx_hz[rate] = hz;
    g_tx_clkdiv[rate] = ((uint32_t)div_frac8 << PIO_SM0_CLKDIV_FRAC_LSB) | (div_int << PIO_SM0_CLKDIV_INT_LSB);
}

uint32_t mapledev_tx_hz(unsigned rate) { return rate < MAPLEDEV_TX_RATES ? g_tx_hz[rate] : 0; }

// The data loop's delay fields by instruction index; sync and tail keep the assembled timing.
enum { D_HOLD, D_HOLD_1, D_SETTLE, D_MID, D_MID_1 };
static const struct { uint8_t idx, kind; } k_tx_delay[] = {
    { 9, D_MID_1 }, { 11, D_SETTLE }, { 12, D_HOLD }, { 13, D_MID }, { 14, D_SETTLE },
    { 15, D_HOLD }, { 16, D_MID }, { 17, D_SETTLE }, { 18, D_HOLD_1 },
};
#define TX_NDELAY (sizeof k_tx_delay / sizeof k_tx_delay[0])
#define TX_TIMING(h, s, m) ((uint32_t)(h) | ((uint32_t)(s) << 8) | ((uint32_t)(m) << 16))
#define TX_DEFAULT TX_TIMING(maple_tx_HOLD, maple_tx_SETTLE, maple_tx_MID)

static volatile uint32_t g_tx_timing[MAPLEDEV_TX_RATES] = { TX_DEFAULT, TX_DEFAULT };
static uint32_t g_tx_loaded = TX_DEFAULT;   // core1 only: what instruction memory holds now
static bool g_tx_patchable;

static unsigned delay_of(uint32_t t, unsigned kind) {
    unsigned h = t & 0xffU, s = (t >> 8) & 0xffU, m = (t >> 16) & 0xffU;
    switch (kind) {
    case D_HOLD:   return h;
    case D_HOLD_1: return h - 1U;
    case D_SETTLE: return s;
    case D_MID:    return m;
    default:       return m - 1U;
    }
}

// .side_set 1 leaves bits 11:8 for the delay. Refuses if the program no longer matches the table.
static void tx_patch_check(void) {
    g_tx_patchable = true;
    for (unsigned i = 0; i < TX_NDELAY; i++) {
        uint16_t ins = maple_tx_program_instructions[k_tx_delay[i].idx];
        if (((ins >> 8) & 0xfU) != delay_of(TX_DEFAULT, k_tx_delay[i].kind)) { g_tx_patchable = false; }
    }
}

static void __not_in_flash_func(tx_load_timing)(uint32_t t) {
    for (unsigned i = 0; i < TX_NDELAY; i++) {
        uint16_t ins = maple_tx_program_instructions[k_tx_delay[i].idx];
        if ((ins & 0xe000U) == 0) { ins = (uint16_t)(ins + g_off_tx); }  // JMP: relocate as pio_add_program did
        ins = (uint16_t)((ins & ~0x0f00U) | (delay_of(t, k_tx_delay[i].kind) << 8));
        g_pio->instr_mem[g_off_tx + k_tx_delay[i].idx] = ins;
    }
    g_tx_loaded = t;
}

bool mapledev_set_tx_timing(unsigned rate, unsigned hold, unsigned settle, unsigned mid) {
    if (rate >= MAPLEDEV_TX_RATES || !g_tx_patchable) { return false; }
    if (hold < 1 || hold > 15 || mid < 1 || mid > 15 || settle > 15) { return false; }
    g_tx_timing[rate] = TX_TIMING(hold, settle, mid);   // one word, so core1 never sees half of it
    return true;
}

void mapledev_tx_timing(unsigned rate, unsigned *hold, unsigned *settle, unsigned *mid) {
    uint32_t t = g_tx_timing[rate < MAPLEDEV_TX_RATES ? rate : 0];
    *hold = t & 0xffU; *settle = (t >> 8) & 0xffU; *mid = (t >> 16) & 0xffU;
}

// TX SM is parked between frames, so instruction memory can be rewritten here safely.
void __not_in_flash_func(mapledev_tx_select)(unsigned rate) {
    if (rate >= MAPLEDEV_TX_RATES) { return; }
    uint32_t t = g_tx_timing[rate];
    if (g_tx_patchable && t != g_tx_loaded) { tx_load_timing(t); }
    if (g_tx_clkdiv[rate]) { g_pio->sm[g_sm_tx].clkdiv = g_tx_clkdiv[rate]; }
}
