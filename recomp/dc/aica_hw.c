// The AICA from the SH-4 over G2: channel registers written directly, rings in sound RAM. KOS leaves the ARM
// spinning on its idle loop at sound RAM 0, which owns no channel.
#include <kos.h>
#include <dc/g2bus.h>

#include "aica_voice.h"
#include "tmu1.h"

#define SND(r) (0xA0700000u + (r))
#define CHN(ch, r) (0xA0700000u + (ch) * 0x80u + (r))
#define RAM(o) (0xA0800000u + (o))
#define KYONEX 0x8000u
#define KYONB 0x4000u
#define LPCTL 0x0200u
#define MONITOR_NS 30000u  // CA follows MSLC only at the next 44.1 kHz sample; 20 nops read the last channel's
#define RING_Q32 ((uint64_t)AV_RING << 32)

static uint32_t reg0[AV_CHANNELS], ring_of[AV_CHANNELS];

// Play positions run on the SH-4 clock at each channel's rate; one channel a list is read back from CA
// (measured 0.25% fast at OCT -2 FNS 1023, well inside the 60 ms lead)
static struct { uint64_t pos_q32, at_ns, step_q32; } play[AV_CHANNELS];
static uint32_t read_next;
static int read_done;

static uint64_t step_of(uint32_t freq_reg) {
    int oct = (int)(((freq_reg >> 11) & 15) ^ 8) - 8;
    uint64_t s = (uint64_t)(1024 + (freq_reg & 1023)) * 44100u << 22;
    return (oct >= 0 ? s << oct : s >> -oct) / 1000000000u;
}

static void play_advance(uint32_t ch, uint64_t now) {
    play[ch].pos_q32 = (play[ch].pos_q32 + (now - play[ch].at_ns) * play[ch].step_q32) % RING_Q32;
    play[ch].at_ns = now;
}

static void wr8(const uint32_t *addr, const uint32_t *val, int n) {
    g2_ctx_t ctx = g2_lock();
    for (int i = 0; i < n; i++) {
        if (!(i & 7)) g2_fifo_wait();
        g2_write_32_raw(addr[i], val[i]);
    }
    g2_unlock(ctx);
}

// Resident voices' events, each applied AV_LEAD_MS after it was queued, from a 1 kHz TMU1 tick
#define EVQ 512u  // 60 ms of a busy song is about 130; a full queue applies at once
#define EV_HZ 1000
static struct { struct av_ev e; uint64_t due; } evq[EVQ];
static volatile uint32_t ev_r, ev_w;
static volatile uint16_t ev_pending[AV_CHANNELS];

static void ev_apply(const struct av_ev *e) {
    uint32_t ch = e->ch;
    if (e->kind == AV_EV_START) {
        reg0[ch] = LPCTL | ((e->sa >> 16) & 0x7F);
        const uint32_t a[] = { CHN(ch, 0x00), CHN(ch, 0x04), CHN(ch, 0x08), CHN(ch, 0x0C), CHN(ch, 0x10), CHN(ch, 0x14),
                               CHN(ch, 0x18), CHN(ch, 0x1C), CHN(ch, 0x20), CHN(ch, 0x24), CHN(ch, 0x28) };
        const uint32_t v[] = { reg0[ch], e->sa & 0xFFFF, e->lsa, e->lea, 0x1F, 0x1F, e->freq_reg, 0, 0,
                               0x0F00 | e->dipan, e->tl << 8 | 0x24 };
        wr8(a, v, 11);
        reg0[ch] |= KYONB;
    } else if (e->kind == AV_EV_PARAMS) {
        const uint32_t a[] = { CHN(ch, 0x18), CHN(ch, 0x24), CHN(ch, 0x28) };
        const uint32_t v[] = { e->freq_reg, 0x0F00 | e->dipan, e->tl << 8 | 0x24 };
        wr8(a, v, 3);
        return;
    } else {
        reg0[ch] &= ~KYONB;
    }
    g2_ctx_t ctx = g2_lock();
    g2_fifo_wait();
    g2_write_32_raw(CHN(ch, 0), reg0[ch]);
    g2_write_32_raw(CHN(ch, 0), reg0[ch] | KYONEX);
    g2_unlock(ctx);
}

#define DRY_NS (200ull * 1000000ull)  // no list for this long: the N64's AI has run dry, so the AICA goes quiet
static volatile uint64_t list_ns, unmute_ns;
static volatile int muted;

void aica_hw_list_done(void) {
    int old = irq_disable();
    list_ns = timer_ns_gettime64();
    if (muted && !unmute_ns) unmute_ns = list_ns + AV_LEAD_MS * 1000000ull;  // when this list's samples start to play
    irq_restore(old);
}

static void tick(irq_context_t *ctx) {
    (void)ctx;
    uint64_t now = timer_ns_gettime64();
    while (ev_r != ev_w && evq[ev_r % EVQ].due <= now) {
        const struct av_ev *e = &evq[ev_r % EVQ].e;
        ev_apply(e);
        ev_pending[e->ch]--;
        ev_r++;
    }
    if (!muted && list_ns && now - list_ns > DRY_NS) {
        g2_write_32(SND(0x2800), 0);
        muted = 1, unmute_ns = 0;
    } else if (muted && unmute_ns && now >= unmute_ns) {
        g2_write_32(SND(0x2800), 0x000F);
        muted = 0;
    }
}

void aica_hw_event(const struct av_ev *e) {
    int old = irq_disable();
    if (ev_w - ev_r == EVQ) {
        ev_apply(e);
    } else {
        evq[ev_w % EVQ].e = *e;
        evq[ev_w % EVQ].due = timer_ns_gettime64() + AV_LEAD_MS * 1000000ull;
        ev_w++;
        ev_pending[e->ch]++;
    }
    irq_restore(old);
}

int aica_hw_idle(uint32_t ch) { return !ev_pending[ch]; }

void aica_hw_loop(uint32_t ch, uint32_t lsa, uint32_t lea) {
    int old = irq_disable();
    for (uint32_t i = ev_r; i != ev_w; i++)
        if (evq[i % EVQ].e.ch == ch && evq[i % EVQ].e.kind == AV_EV_START) evq[i % EVQ].e.lsa = lsa, evq[i % EVQ].e.lea = lea;
    const uint32_t a[] = { CHN(ch, 0x08), CHN(ch, 0x0C) }, v[] = { lsa, lea };
    wr8(a, v, 2);
    irq_restore(old);
}

void aica_hw_init(void) {
    spu_reset_chans();
    g2_write_32(SND(0x2800), 0x000F);
    tmu1_use(tick, EV_HZ);
}

void aica_hw_read(uint32_t off, void *dst, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t a = off + i;
        ((uint8_t *)dst)[i] = (uint8_t)(g2_read_32(RAM(a & ~3u)) >> 8 * (a & 3));
    }
}

// A list's ring writes as one G2 DMA chain, each transfer started from the last one's completion interrupt
struct xfer { const void *src; uint32_t dst, len; };
#define QUEUE (8 * AV_CHANNELS + 64)  // per voice: a lead or fill, its FIFO and a tail, each split at the ring's wrap; 64 for aica_hw_write
static struct xfer queue[2][QUEUE];
static volatile uint32_t chain_next, chain_len, chain_busy;
static uint32_t fill, nfill;

static void chain(void *unused) {
    (void)unused;
    if (chain_next == chain_len) { chain_busy = 0; return; }
    const struct xfer *x = &queue[fill ^ 1][chain_next++];
    if (spu_dma_transfer((void *)x->src, x->dst, x->len, 0, chain, 0) < 0) chain_busy = 0;
}

void aica_hw_write(uint32_t off, const void *src, uint32_t bytes) {
    if (nfill == QUEUE) return;
    dcache_flush_range((uintptr_t)src, bytes);
    queue[fill][nfill++] = (struct xfer){ src, off, bytes };
}

void aica_hw_ring_write(uint32_t ch, uint32_t pos, const void *src, uint32_t bytes) {
    aica_hw_write(ring_of[ch] + 2 * pos, src, bytes);
}

void aica_hw_setup(uint32_t ch, uint32_t ring_off) {
    ring_of[ch] = ring_off;
    reg0[ch] = LPCTL | ((ring_off >> 16) & 0x7F);
    const uint32_t a[] = { CHN(ch, 0x00), CHN(ch, 0x04), CHN(ch, 0x08), CHN(ch, 0x0C), CHN(ch, 0x10), CHN(ch, 0x14),
                           CHN(ch, 0x1C), CHN(ch, 0x20), CHN(ch, 0x24), CHN(ch, 0x28) };
    const uint32_t v[] = { reg0[ch], ring_off & 0xFFFF, 0, AV_RING, 0x1F, 0x1F, 0, 0, 0x0F00, 0xFF24 };
    wr8(a, v, 10);
}

void aica_hw_params(uint32_t ch, uint32_t freq_reg, uint32_t tl, uint32_t dipan) {
    const uint32_t a[] = { CHN(ch, 0x18), CHN(ch, 0x24), CHN(ch, 0x28) };
    const uint32_t v[] = { freq_reg, 0x0F00 | dipan, tl << 8 | 0x24 };
    play_advance(ch, timer_ns_gettime64());
    play[ch].step_q32 = step_of(freq_reg);
    wr8(a, v, 3);
}

void aica_hw_keys(unsigned long long on, unsigned long long off) {
    if (!(on | off)) return;
    g2_ctx_t ctx = g2_lock();
    int n = 0, last = 0;
    for (int ch = 0; ch < AV_CHANNELS; ch++) {
        if (!((on | off) >> ch & 1)) continue;
        reg0[ch] = (on >> ch & 1) ? reg0[ch] | KYONB : reg0[ch] & ~KYONB;
        if (!(n++ & 7)) g2_fifo_wait();
        g2_write_32_raw(CHN(ch, 0), reg0[ch]);
        last = ch;
    }
    g2_fifo_wait();
    g2_write_32_raw(CHN(last, 0), reg0[last] | KYONEX);
    uint64_t now = timer_ns_gettime64();
    g2_unlock(ctx);
    for (int ch = 0; ch < AV_CHANNELS; ch++)
        if (on >> ch & 1) play[ch].pos_q32 = 0, play[ch].at_ns = now;
}

uint32_t aica_hw_pos(uint32_t ch) {
    if (!read_done && ch >= read_next) {
        g2_write_32(SND(0x280C), (ch & 0x3F) << 8);
        uint64_t t0 = timer_ns_gettime64();
        while (timer_ns_gettime64() - t0 < MONITOR_NS) {}
        uint32_t ca = g2_read_32(SND(0x2814)) & (AV_RING - 1);
        play[ch].pos_q32 = (uint64_t)ca << 32 | 0x80000000u;
        play[ch].at_ns = timer_ns_gettime64();
        read_next = ch + 1, read_done = 1;
    } else {
        play_advance(ch, timer_ns_gettime64());
    }
    return (uint32_t)(play[ch].pos_q32 >> 32);
}

void aica_hw_flush(void) {
    if (!read_done) read_next = 0;
    read_done = 0;
    while (chain_busy) {}
    if (!nfill) return;
    chain_next = 0, chain_len = nfill;
    fill ^= 1, nfill = 0;
    chain_busy = 1;
    chain(0);
}
