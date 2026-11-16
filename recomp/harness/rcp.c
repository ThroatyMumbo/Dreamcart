#include "rcp.h"
#include "os.h"
#include "tmpl.h"
#include "cartinfo.h"
#include "rsp_audio.h"
#include "rsp_gfx.h"
#include "pi_ledger.h"
#include "ovl.h"
#include "pif.h"
#include "flashram.h"
#include "rsp_task.h"

#define RDRAM_SIZE RDRAM_BYTES
#define VI_NTSC_HZ 48681812ull

enum { B_SP, B_DP, B_DPS, B_MI, B_VI, B_AI, B_PI, B_RI, B_SI, NBLOCK };
static uint32_t regs[NBLOCK][16];
static uint32_t sp_pc, sp_sem;
static uint32_t pif_ram[16];

static uint32_t mi_intr, mi_mask;
static uint32_t sp_status = 1;  // halted at reset
static int vi_on;
static uint32_t vi_period;
static unsigned long long vi_next;
static uint32_t vi_fields;
uint32_t rcp_vi_intrs;
struct rcp_hooks rcp_hooks;

#define NSCHED 8
static unsigned long long sched_when[NSCHED];
static uint32_t sched_bits[NSCHED];

// PI timing model, not measured: ~5 MB/s cart DMA (about 9 counts a byte) after a fixed setup
#define PI_SETUP_COUNTS 0x400u
#define PI_COUNTS_PER_BYTE 9u
#define PI_SPIN_STEP 0x100u  // a PI_STATUS read while busy lets time pass, like a Count read
#define PI_CHUNK 4096u
static rcp_cart_fn cart_read_fn;
static uint32_t cart_size;
static int pi_busy;

// SI timing model, not measured: one 64-byte PIF transfer with its joybus commands
#define SI_DMA_COUNTS 6000u
static int si_busy;

static void si_dma(int to_pif) {
    uint8_t b[64];
    uint32_t dram = regs[B_SI][0] & 0x00FFFFF8;
    if (si_busy) os_die("rcp: SI DMA started while another is in flight", dram);
    if (dram + 64 > RDRAM_SIZE) os_die("rcp: SI DMA past the end of RDRAM", dram);
    if (to_pif) {
        for (int i = 0; i < 64; i++) b[i] = M8(0x80000000u | (dram + i));
        pif_write(b);
    } else {
        for (int i = 0; i < 64; i++) b[i] = (uint8_t)(pif_ram[i >> 2] >> (24 - 8 * (i & 3)));
        pif_read(b);
        for (int i = 0; i < 64; i++) M8(0x80000000u | (dram + i)) = b[i];
    }
    for (int i = 0; i < 16; i++)
        pif_ram[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | b[4 * i + 2] << 8 | b[4 * i + 3];
    unsigned long long at = os_now() + SI_DMA_COUNTS;
    si_busy = 1;
    rcp_schedule(at, MI_INTR_SI);
}
static uint8_t pi_buf[PI_CHUNK] __attribute__((aligned(4)));

static void sp_tick(unsigned long long now);
static int sp_running;
static unsigned long long sp_done_at;

// AI: a two-buffer FIFO; ai[0] plays from ai_start for ai_dur counts, and each buffer interrupts as it starts
struct ai_buf { uint32_t dram, len; };
static struct ai_buf ai[2];
static int ai_n;
uint32_t rcp_ai_dry, rcp_ai_frames;
static unsigned long long ai_start;
static uint32_t ai_dur;

static void ai_play(unsigned long long at) {
    uint32_t dac = regs[B_AI][4] & 0x3FFF;
    if (!dac) os_die("rcp: AI_LEN before AI_DACRATE was set", ai[0].len);
    ai_start = at;
    ai_dur = (uint32_t)((unsigned long long)(ai[0].len / 4) * OS_COUNT_HZ * (dac + 1) / VI_NTSC_HZ);
    mi_intr |= MI_INTR_AI;
}

static void ai_tick(unsigned long long now) {
    while (ai_n && now >= ai_start + ai_dur) {
        unsigned long long end = ai_start + ai_dur;
        ai[0] = ai[1];
        if (--ai_n) ai_play(end);
        else rcp_ai_dry++;
    }
}

static void ai_push(uint32_t v) {
    uint32_t len = v & 0x3FFF8;
    if (ai_n == 2) os_die("rcp: AI_LEN write with the AI FIFO full", len);
    if (!len) return;
    ai[ai_n].dram = regs[B_AI][0] & 0x00FFFFF8;
    ai[ai_n].len = len;
    rcp_ai_frames += len / 4;
    if (ai[ai_n].dram + len > RDRAM_SIZE) os_die("rcp: AI buffer past the end of RDRAM", ai[ai_n].dram);
    if (ai_n++ == 0) ai_play(os_now());
}

uint32_t rcp_ai_hz(void) {
    uint32_t dac = regs[B_AI][4] & 0x3FFF;
    return dac ? (uint32_t)(VI_NTSC_HZ / (dac + 1)) : 32000u;
}

static uint32_t ai_remaining(void) {
    if (!ai_n) return 0;
    unsigned long long played = (os_now() - ai_start) * ai[0].len / ai_dur;
    return ai[0].len - ((uint32_t)played & ~7u);
}

void rcp_set_cart(rcp_cart_fn read, uint32_t size) {
    cart_read_fn = read;
    cart_size = size;
}

static rcp_cart_fn cart_peek_fn;
void rcp_set_cart_peek(rcp_cart_fn read) { cart_peek_fn = read; }

int rcp_cart_read(uint32_t off, void *dst, uint32_t len) {
    rcp_cart_fn f = cart_peek_fn ? cart_peek_fn : cart_read_fn;
    return f && f(off, dst, len);
}

static void compute_vi_period(void) {
    uint32_t vs = regs[B_VI][6] & 0x3FF, hs = regs[B_VI][7] & 0xFFF;
    if (!vs) vs = 525;
    if (!hs) hs = 3093;
    vi_period = (uint32_t)((unsigned long long)((vs + 1) / 2) * (hs + 1) * OS_COUNT_HZ / VI_NTSC_HZ);
}

uint32_t rcp_vi_period(void) {
    if (!vi_period) compute_vi_period();
    return vi_period;
}

uint32_t rcp_vi_reg(int r) { return regs[B_VI][r & 15]; }
uint32_t rcp_mi_pending(void) { return mi_intr & mi_mask; }
uint32_t rcp_mi_mask(void) { return mi_mask; }

void rcp_schedule(unsigned long long when, uint32_t bits) {
    for (int i = 0; i < NSCHED; i++) {
        if (sched_bits[i]) continue;
        sched_when[i] = when;
        sched_bits[i] = bits;
        return;
    }
    os_die("rcp: completion queue full", bits);
}

void rcp_tick(unsigned long long now) {
    ai_tick(now);
    sp_tick(now);
    if (vi_on && now >= vi_next) {
        unsigned long long n = (now - vi_next) / vi_period + 1;
        mi_intr |= MI_INTR_VI;
        vi_next += n * vi_period;
        vi_fields += (uint32_t)n;
        rcp_vi_intrs++;
        if (rcp_hooks.vi) rcp_hooks.vi(vi_fields);
    }
    for (int i = 0; i < NSCHED; i++) {
        if (sched_bits[i] && now >= sched_when[i]) {
            mi_intr |= sched_bits[i];
            if (sched_bits[i] & MI_INTR_PI) pi_busy = 0;
            if (sched_bits[i] & MI_INTR_SI) si_busy = 0;
            sched_bits[i] = 0;
        }
    }
}

static uint8_t pib[256];
static uint32_t pib_off = ~0u, pib_n;

static uint8_t pi_rom_byte(uint32_t at) {
    if (cart_size && at >= cart_size) return 0;
    if (at - pib_off >= pib_n) {
        pib_n = cart_size && cart_size - at < sizeof pib ? cart_size - at : sizeof pib;
        if (!cart_read_fn(at, pib, pib_n)) os_die("rcp: cart read failed at ROM offset", at);
        pib_off = at;
    }
    return pib[at - pib_off];
}

// An odd length or a DRAM address off 8 bytes: ares's PI::dmaWrite, block by block through the PI's 128-byte buffer
static void pi_dma_quirks(uint32_t dram, uint32_t off, uint32_t length) {
    uint8_t mem[130];
    int32_t len = (int32_t)length, max = 128, first = 1;
    while (len > 0) {
        int32_t mis = dram & 7, row = 0x800 - (dram & 0x7FF), blk = max - mis < row ? max - mis : row;
        int32_t cur = len < blk ? len : blk;
        if (dram + cur > RDRAM_SIZE) os_die("rcp: PI DMA past the end of RDRAM", dram);
        for (int32_t i = 0; i < cur; i += 2, off += 2, len -= 2) mem[i] = pi_rom_byte(off), mem[i + 1] = pi_rom_byte(off + 1);
        if (first && cur < 127 - mis)
            for (int32_t i = 0; i < cur - mis; i++) M8(0x80000000u | dram++) = mem[i];
        else
            for (int32_t i = 0; i < cur - mis; i += 2) M8(0x80000000u | dram++) = mem[i], M8(0x80000000u | dram++) = mem[i + 1];
        dram = (dram + 7) & ~7u;
        first = 0;
        max = row < 8 ? 128 - mis : 128;
    }
}

static void pi_done_at(uint32_t len) {
    pi_busy = 1;
    rcp_schedule(os_now() + PI_SETUP_COUNTS + (unsigned long long)len * PI_COUNTS_PER_BYTE, MI_INTR_PI);
}

// The save chip's DMAs, either way; only FlashRAM is modeled on domain 2
static void pi_flash_dma(uint32_t v, int to_cart) {
    uint32_t dram = regs[B_PI][0] & 0x00FFFFFE, cart = regs[B_PI][1] & ~1u, len = (v & 0x00FFFFFF) + 1;
    if (pi_busy) os_die("rcp: PI DMA started while another is in flight", cart);
    if (to_cart) flash_dma_write(dram, cart - FLASH_BASE, len);
    else flash_dma_read(dram, cart - FLASH_BASE, len);
    pi_done_at(len);
}

static void pi_dma(uint32_t v) {
    uint32_t dram = regs[B_PI][0] & 0x00FFFFFE, cart = regs[B_PI][1] & ~1u, len = (v & 0x00FFFFFF) + 1;
    if (flash_claims(cart)) { pi_flash_dma(v, 0); return; }
    if (pi_busy) os_die("rcp: PI DMA started while another is in flight", cart);
    if (cart < 0x10000000 || cart >= 0x1FC00000)
        os_die("rcp: PI DMA outside cart ROM (SRAM, FlashRAM and 64DD are the save layer)", cart);
    uint32_t off = cart - 0x10000000;
    if (dram + len > RDRAM_SIZE) os_die("rcp: PI DMA past the end of RDRAM", dram);
    if (cart_size && off + len > cart_size) os_die("rcp: PI DMA past the end of the ROM", off);
    if (!cart_read_fn) os_die("rcp: PI DMA with no cart backend", off);
    if ((dram & 7) || (len & 1)) pi_dma_quirks(dram, off, len);
    else for (uint32_t done = 0; done < len;) {
        uint32_t n = len - done < PI_CHUNK ? len - done : PI_CHUNK, i = 0;
        if (!cart_read_fn(off + done, pi_buf, n)) os_die("rcp: cart read failed at ROM offset", off + done);
        uint32_t a = 0x80000000u | (dram + done);
        for (; i + 4 <= n; i += 4)
            M32(a + i) = (uint32_t)pi_buf[i] << 24 | (uint32_t)pi_buf[i + 1] << 16 | pi_buf[i + 2] << 8 | pi_buf[i + 3];
        for (; i < n; i++) M8(a + i) = pi_buf[i];
        done += n;
    }
    unsigned long long at = os_now() + PI_SETUP_COUNTS + (unsigned long long)len * PI_COUNTS_PER_BYTE;
    pi_busy = 1;
    rcp_schedule(at, MI_INTR_PI);
    ov_dma(off, dram, len);
    pi_ledger_add(off, dram, len);
    if (rcp_hooks.pi) rcp_hooks.pi(off, dram, len, at);
}

unsigned long long rcp_next_event(unsigned long long now) {
    unsigned long long t = vi_on ? vi_next : RCP_NO_EVENT;
    if (ai_n && ai_start + ai_dur < t) t = ai_start + ai_dur;
    if (sp_running && sp_done_at < t) t = sp_done_at;
    for (int i = 0; i < NSCHED; i++)
        if (sched_bits[i] && sched_when[i] < t) t = sched_when[i];
    return t < now ? now : t;
}

static uint32_t vi_current(void) {
    uint32_t p = rcp_vi_period(), vs = regs[B_VI][6] & 0x3FF;
    unsigned long long now = os_now();
    uint32_t phase = (uint32_t)(now % p);
    if (!vs) vs = 525;
    return (uint32_t)((unsigned long long)phase * (vs + 1) / p) & ~1u;
}

static void mask_write(uint32_t v) {
    for (int b = 0; b < 6; b++) {
        if (v & (1u << (2 * b))) mi_mask &= ~(1u << b);
        if (v & (2u << (2 * b))) mi_mask |= 1u << b;
    }
}

// SP memory word-swapped like the RDRAM view (bytes at ^3): DMEM 0x000-0xFFF, IMEM 0x1000-0x1FFF
static uint8_t spmem[0x2000] __attribute__((aligned(8)));

static uint32_t sp_word(uint32_t a) { return *(uint32_t *)(spmem + (a & 0x1FFC)); }
static void sp_set_word(uint32_t a, uint32_t v) { *(uint32_t *)(spmem + (a & 0x1FFC)) = v; }

void rcp_boot_spmem(uint32_t fill, uint32_t imem0) {
    for (uint32_t a = 0; a < sizeof spmem; a += 4) sp_set_word(a, fill);
    sp_set_word(0x1000, imem0);
}

// RSP task timing model, not measured: a fixed start plus a cost per audio command
#define RSP_SETUP_COUNTS 2000u
#define RSP_COUNTS_PER_CMD 400u
#define RSP_COUNTS_PER_MB 4000u  // a JPEG macroblock's IDCT and color conversion

// Graphics task model, not measured: per display-list command, vertex and triangle; the RDP finishes after
#define GFX_SETUP_COUNTS 4000u
#define GFX_COUNTS_PER_CMD 40u
#define GFX_COUNTS_PER_VTX 100u
#define GFX_COUNTS_PER_TRI 300u
#define DP_TAIL_COUNTS 2000u

// SP DMA completes at once: rows of (len & 0xFFF) + 1 bytes, count rows, skip bytes between rows in RDRAM
static void sp_dma(uint32_t v, int to_sp) {
    uint32_t len = ((v & 0xFFF) | 7) + 1, count = ((v >> 12) & 0xFF) + 1, skip = (v >> 20) & 0xFF8;
    uint32_t mem = regs[B_SP][0] & 0x1FF8, dram = regs[B_SP][1] & 0x00FFFFF8;
    for (uint32_t row = 0; row < count; row++, dram += len + skip) {
        if (dram + len > RDRAM_SIZE) os_die("rcp: SP DMA past the end of RDRAM", dram);
        for (uint32_t i = 0; i < len; i += 4) {  // both sides hold host-order words and the DMA is 8-byte aligned
            uint32_t *s = (uint32_t *)(spmem + ((mem & 0x1000) | ((mem + i) & 0xFFF)));
            if (to_sp) *s = M32(0x80000000u | (dram + i));
            else M32(0x80000000u | (dram + i)) = *s;
        }
        mem = (mem & 0x1000) | ((mem + len) & 0xFFF);
    }
}


// A graphics task runs on the RSP's fiber, so interrupts due mid-list reach the CPU and the game can yield it
#define OS_TASK_YIELDED 0x1u
#define SP_SIG0 0x80u  // osSpTaskYield's request
#define SP_SIG1 0x100u  // the task yielded
enum { GFX_IDLE, GFX_RUNNING, GFX_PARKED, GFX_YIELDED };
static int gfx_state;
static uint32_t gfx_pc;
static unsigned long long gfx_t0;
static struct gfx_stats gfx_st;
static uint8_t gfx_dmem[0x1000] __attribute__((aligned(8)));  // DMEM at the start: a yield's restart rewrites the header

static void gfx_body(void) {
    rsp_gfx_task(gfx_dmem, &gfx_st);
    const struct gfx_stats *st = &gfx_st;
    gfx_state = GFX_IDLE;
    sp_done_at = gfx_t0 + GFX_SETUP_COUNTS + (unsigned long long)st->cmds * GFX_COUNTS_PER_CMD
                 + (unsigned long long)st->vtx * GFX_COUNTS_PER_VTX + (unsigned long long)st->tris * GFX_COUNTS_PER_TRI;
    if (st->fullsync) rcp_schedule(sp_done_at + DP_TAIL_COUNTS, MI_INTR_DP);
    if (rcp_hooks.gfx_task) rcp_hooks.gfx_task(spmem, gfx_pc, st, sp_done_at);
}

// Parked tasks stop at a command boundary, so a yield asked for then takes effect at once
static void gfx_yield(void) {
    sp_running = 0;
    sp_status |= 0x3 | SP_SIG1;
    if (sp_status & 0x40) mi_intr |= MI_INTR_SP;
    gfx_state = GFX_YIELDED;
}

static void gfx_poll(void) {
    if (sp_status & SP_SIG0) {
        gfx_tmem_flush();
        gfx_yield();
        os_cop_park();
    } else if (os_cop_due()) {
        gfx_tmem_flush();
        gfx_state = GFX_PARKED;
        os_cop_park();
    }
}

static void gfx_slice(void) {
    gfx_state = GFX_RUNNING;
    if (rcp_hooks.task_start) rcp_hooks.task_start(1);
    if (!os_cop_run() && rcp_hooks.task_pause) rcp_hooks.task_pause(1);
}

int rcp_rsp_parked(void) { return gfx_state == GFX_PARKED; }

int rcp_rsp_idle(void) {
    if (gfx_state != GFX_PARKED) return 0;
    gfx_slice();
    return 1;
}

static void sp_start(void) {
    uint32_t type = sp_word(0xFC0);
    if (sp_running) os_die("rcp: RSP started while a task is running", type);
    if (type == 1) {
        int resume = (sp_word(0xFC4) & OS_TASK_YIELDED) != 0;
        if (resume != (gfx_state == GFX_YIELDED))
            os_die(resume ? "rcp: yielded graphics task restarted with none suspended"
                          : "rcp: new graphics task while a yielded one is suspended", sp_word(0xFC4));
        if (!resume) {
            rsp_gfx_poll = gfx_poll;
            gfx_pc = sp_pc;
            gfx_t0 = os_now();
            __builtin_memcpy(gfx_dmem, spmem, sizeof gfx_dmem);
            os_cop_start(gfx_body);
        }
        sp_status &= ~0x3u;
        sp_running = 1;
        sp_done_at = RCP_NO_EVENT;
        gfx_state = GFX_PARKED;
        if (os_rsp_outranks_running()) gfx_slice();
        return;
    }
    if (rcp_hooks.task_start) rcp_hooks.task_start(type);
    if (type != 2) {
        uint32_t n = rsp_other_task(spmem, type);
        sp_status &= ~0x3u;
        sp_running = 1;
        sp_done_at = os_now() + RSP_SETUP_COUNTS + (unsigned long long)n * RSP_COUNTS_PER_MB;
        return;
    }
    uint32_t cmds = rsp_audio_task(spmem, rcp_ai_hz());
    sp_status &= ~0x3u;  // running: neither halted nor broken until the modeled end
    sp_running = 1;
    sp_done_at = os_now() + RSP_SETUP_COUNTS + (unsigned long long)cmds * RSP_COUNTS_PER_CMD;
    if (rcp_hooks.audio_task) rcp_hooks.audio_task(sp_word(0xFF0), cmds, sp_done_at);
}

static void sp_tick(unsigned long long now) {
    if (!sp_running || now < sp_done_at) return;
    sp_running = 0;
    sp_status |= 0x3 | 0x200;  // halt, broke, SIG2 = task done
    if (sp_status & 0x40) mi_intr |= MI_INTR_SP;  // interrupt on break, set by osSpTaskLoad/StartGo
}

static void sp_status_write(uint32_t v) {
    if (v & 0x1) {
        sp_status &= ~0x1u;
        sp_start();
    }
    if (v & 0x2) sp_status |= 0x1;
    if (v & 0x4) sp_status &= ~0x2u;
    if (v & 0x8) mi_intr &= ~MI_INTR_SP;
    if (v & 0x10) mi_intr |= MI_INTR_SP;
    if (v & 0x20) sp_status &= ~0x20u;
    if (v & 0x40) sp_status |= 0x20;
    if (v & 0x80) sp_status &= ~0x40u;
    if (v & 0x100) sp_status |= 0x40;
    for (int n = 0; n < 8; n++) {
        if (v & (0x200u << (2 * n))) sp_status &= ~(0x80u << n);
        if (v & (0x400u << (2 * n))) sp_status |= 0x80u << n;
    }
    if (gfx_state == GFX_PARKED && (sp_status & SP_SIG0)) gfx_yield();
}

static int reg_index(uint32_t phys, uint32_t *blk) {
    uint32_t b = (phys >> 20) & 0xF, r = (phys & 0xFFFF) >> 2;
    *blk = b;
    if (b >= NBLOCK || r >= 16 || (phys & 0xF0000) != (b == B_SP ? 0x40000u : 0)) return -1;
    return (int)r;
}

static uint32_t reg_rd(uint32_t phys) {
    uint32_t blk;
    int r = reg_index(phys, &blk);
    if (phys == 0x04080000) return sp_pc;
    if (r < 0) os_die("rcp: read of an unmapped register", phys);
    switch (blk) {
    case B_SP:
        if (r == 4) return sp_status;
        if (r == 5 || r == 6) return 0;
        if (r == 7) { uint32_t s = sp_sem; sp_sem = 1; return s; }
        break;
    case B_MI:
        if (r == 1) return 0x02020102;
        if (r == 2) return mi_intr;
        if (r == 3) return mi_mask;
        break;
    case B_VI:
        if (r == 4) return vi_current();
        break;
    case B_AI:
        if (r == 1) return ai_remaining();
        if (r == 3) return (ai_n == 2 ? 0x80000001u : 0) | (ai_n ? 0x40000000u : 0);  // FIFO full, busy
        break;
    case B_PI:
        if (r == 4) {
            if (pi_busy) os_advance(PI_SPIN_STEP);
            return pi_busy ? 1u : 0;  // PI_STATUS_DMA_BUSY
        }
        break;
    case B_SI:
        if (r == 6) {
            if (si_busy) os_advance(PI_SPIN_STEP);
            return (si_busy ? 1u : 0) | (mi_intr & MI_INTR_SI ? 0x1000u : 0);  // DMA busy, interrupt
        }
        break;
    }
    return regs[blk][r];
}

static void reg_wr(uint32_t phys, uint32_t v) {
    uint32_t blk;
    int r = reg_index(phys, &blk);
    if (phys == 0x04080000) { sp_pc = v; return; }
    if (r < 0) os_die("rcp: write to an unmapped register", phys);
    switch (blk) {
    case B_SP:
        if (r == 2 || r == 3) { sp_dma(v, r == 2); return; }
        if (r == 4) { sp_status_write(v); return; }
        if (r == 7) { sp_sem = 0; return; }
        break;
    case B_DP:
        if (r == 1) os_die("rcp: DPC_END write (RDP command lists are the next layer)", v);
        break;
    case B_MI:
        if (r == 0 && (v & 0x800)) mi_intr &= ~MI_INTR_DP;
        if (r == 3) { mask_write(v); return; }
        if (r == 2) return;
        break;
    case B_VI:
        if (r == 4) { mi_intr &= ~MI_INTR_VI; return; }
        regs[blk][r] = v;
        if (r == 3 && !vi_on) {
            vi_on = 1;
            vi_next = (os_now() / rcp_vi_period() + 1) * vi_period;
        }
        if (r == 6 || r == 7) {
            uint32_t old = vi_period;
            compute_vi_period();
            if (vi_on && vi_period != old) vi_next = os_now() + vi_period;
        }
        return;
    case B_AI:
        if (r == 1) { ai_push(v); return; }
        if (r == 3) { mi_intr &= ~MI_INTR_AI; return; }
        break;
    case B_PI:
        if (r == 2) {
            if (!flash_claims(regs[B_PI][1] & ~1u)) os_die("rcp: PI_RD_LEN write (RDRAM to cart: SRAM is the save layer)", v);
            pi_flash_dma(v, 1);
            return;
        }
        if (r == 3) { pi_dma(v); return; }
        if (r == 4) { if (v & 2) mi_intr &= ~MI_INTR_PI; return; }
        break;
    case B_SI:
        if (r == 1 || r == 4) { si_dma(r == 4); return; }
        if (r == 6) { mi_intr &= ~MI_INTR_SI; return; }
        break;
    }
    regs[blk][r] = v;
}

uint32_t rcp_rd(uint32_t phys, int size) {
    if (phys < RDRAM_SIZE) {
        uint32_t a = 0x80000000u | phys;
        return size == 4 ? M32(a) : size == 2 ? M16(a) : M8(a);
    }
    if (phys < 0x03F00000) return 0;  // no RDRAM fitted there: osGetMemSize's probe reads 0
    if (size != 4) os_die("rcp: sub-word MMIO read", phys);
    if (phys >= 0x03F00000 && phys < 0x04000000) return 0;
    if (phys >= 0x04000000 && phys < 0x04002000) return sp_word(phys);
    if (phys >= 0x04000000 && phys < 0x04900000) return reg_rd(phys);
    if (phys >= 0x05000000 && phys < 0x08000000) return (phys & 0xFFFF) | phys << 16;  // no 64DD: PI open bus, as ares
    if (flash_claims(phys)) return flash_rd(phys);
    if (phys >= 0x10000000 && phys < 0x10000040) return cart_info.header[(phys >> 2) & 15];
    if (phys >= 0x10000040 && phys < 0x1FC00000 && cart_read_fn) {
        uint8_t w[4];
        if (!cart_read_fn(phys - 0x10000000, w, 4)) os_die("rcp: cart IO read failed at", phys);
        return (uint32_t)w[0] << 24 | (uint32_t)w[1] << 16 | w[2] << 8 | w[3];
    }
    if (phys >= 0x1FC007C0 && phys < 0x1FC00800) return pif_ram[(phys >> 2) & 15];
    if (phys >= 0x1FD00000 && phys < 0x80000000) return (phys & 0xFFFF) | phys << 16;  // cart domain 1 addr 3: open bus (KMC probe)
    os_die("rcp: read outside the modeled bus (cart and PIF are the next layer)", phys);
    return 0;
}

void rcp_wr(uint32_t phys, uint32_t v, int size) {
    if (phys < RDRAM_SIZE) {
        uint32_t a = 0x80000000u | phys;
        if (size == 4) M32(a) = v;
        else if (size == 2) M16(a) = (uint16_t)v;
        else M8(a) = (uint8_t)v;
        return;
    }
    if (phys < 0x03F00000) return;
    if (size != 4) os_die("rcp: sub-word MMIO write", phys);
    if (phys >= 0x03F00000 && phys < 0x04000000) return;
    if (phys >= 0x04000000 && phys < 0x04002000) { sp_set_word(phys, v); return; }
    if (phys >= 0x04000000 && phys < 0x04900000) { reg_wr(phys, v); return; }
    if (phys >= 0x1FC007C0 && phys < 0x1FC00800) { pif_ram[(phys >> 2) & 15] = v; return; }
    if (flash_claims(phys)) { flash_wr(phys, v); return; }
    if (phys >= 0x1FD00000 && phys < 0x80000000) return;
    os_die("rcp: write outside the modeled bus (cart and PIF are the next layer)", phys);
}
