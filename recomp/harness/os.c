#include <string.h>

#include "tmpl.h"
#include "sim.h"
#include "os.h"
#include "rcp.h"
#include "cartinfo.h"
#include "view.h"
#include "hostmmu.h"
#include "osthread.h"
#include "rgn.h"

#define OS_THREAD_TAIL cart_info.thread_tail
#define OS_EVENT_TAB cart_info.event_tab
#define OS_RCP_IM_TABLE cart_info.rcp_im_table
#define OS_DESTROY_THREAD cart_info.destroy_thread

#define RUNQ (OS_THREAD_TAIL + 0x8)
#define ACTIVEQ (OS_THREAD_TAIL + 0xC)
#define RUNNING (OS_THREAD_TAIL + 0x10)
#define ST_RUNNABLE 2
#define ST_RUNNING 4
#define SUSPENDED 1u  // context.pc of a thread parked in its fiber; odd, so never an entry point
#define MI_INTR_MASK_REG 0x0430000Cu

#define NFIBER ((int)OS_NFIBER)
#define STACK_WORDS (OS_FIBER_STACK_BYTES / 4)

struct fiber {
    uint32_t regs[16];
    uint32_t thread;
    int depth;
    struct tctx ctx;
    uint32_t tick;  // the SH-4 return address of its last back-edge poll
};

void fiber_switch(uint32_t *save, const uint32_t *load);
uint32_t t_lookup(uint32_t vram);

static struct fiber fibers[NFIBER];
static uint32_t *stack_lo;  // KOS: must be inside the running thread's declared stack (os_set_stack_pool)
static uint32_t *big_top;
static struct fiber *cur = &fibers[0];

uint32_t os_switches, os_trace_hash;
struct os_hooks os_hooks;

static unsigned long long vclock, cmp_at;
static uint32_t c0_status = 0x34000000u, c0_compare;
static int cmp_pending, cmp_armed;

static unsigned long long (*clock_now)(void);
static void (*clock_wait)(unsigned long long until);

unsigned long long os_now(void) { return vclock; }

void os_set_clock(unsigned long long (*now)(void), void (*wait)(unsigned long long until)) {
    clock_now = now;
    clock_wait = wait;
}

static void catch_up(void) {
    unsigned long long r = clock_now();
    if (r > vclock) os_advance(r - vclock);
}

void os_report(void) {
    rgn_report();
    sim_hex("os vclock", (uint32_t)(vclock >> 32));
    sim_hex("", (uint32_t)vclock);
    sim_hex("switches", os_switches);
    sim_hex("trace", os_trace_hash);
    sim_puts("\n");
    extern uint32_t os_fibers_peak;
    uint32_t deepest = 0, rsp = 0;  // the pool starts zeroed, so the lowest nonzero word is each stack's high-water
    for (int s = 0; s < NFIBER && stack_lo; s++) {
        uint32_t w = 0;
        while (w < STACK_WORDS && !stack_lo[s * STACK_WORDS + w]) w++;
        uint32_t used = 4 * (STACK_WORDS - w);
        if (s == NFIBER - 1) rsp = used;
        else if (used > deepest) deepest = used;
    }
    sim_hex("fibers peak", os_fibers_peak);
    sim_hex(" of", NFIBER - 1);
    sim_hex(" deepest stack", deepest);
    sim_hex(" rsp stack", rsp);
    sim_puts("\n");
}

static int rdram_word(uint32_t a) { return !(a & 3) && a >= RDRAM_LO && a < RDRAM_HI; }

uint32_t os_fn_below(const uint32_t *guest, const uint32_t *host, uint32_t stride, uint32_t n, uint32_t h, uint32_t *at) {
    uint32_t best = 0, a = 0;
    for (uint32_t i = 0; i < n * stride; i += stride)
        if (host[i] <= h && (!best || host[i] > a)) a = host[i], best = guest[i];
    if (at) *at = a;
    return best;
}

static uint32_t fn_below(uint32_t h, uint32_t *at) { return os_fn_below(t_functab_p, t_functab_p + 1, 2, t_functab_n, h, at); }

static uint32_t host_guest_fn(uint32_t h) {
    uint32_t best = rgn_any_guest_fn(h), last;
    if (best || (h & 1)) return best;
    fn_below(~0u, &last);
    return h > last + 0x4000 ? 0 : fn_below(h, NULL);
}

#define CHAIN_MAX 16u

// Host return addresses into translated code on a parked fiber's stack: its real guest call chain, innermost first
static uint32_t chain_from(const struct fiber *f, const uint32_t *hs, uint32_t out[CHAIN_MAX]) {
    uint32_t n = 0, last = 0;
    const uint32_t *top = stack_lo + (f - fibers) * STACK_WORDS;
    for (; stack_lo < hs && hs < top && n < CHAIN_MAX; hs++) {
        uint32_t vram = host_guest_fn(*hs);
        if (vram && vram != last) out[n++] = last = vram;
    }
    return n;
}

static uint32_t host_chain(const struct fiber *f, uint32_t out[CHAIN_MAX]) {
    return f == cur ? 0 : chain_from(f, (const uint32_t *)f->regs[7], out);
}


// Every thread on the active list: state, the queue it waits on and that queue, and its guest stack's return addresses
static void dump_threads(void) {
    if (!OS_THREAD_TAIL) return;
    uint32_t t = M32(ACTIVEQ);
    for (int n = 0; n < 32 && rdram_word(t) && t != OS_THREAD_TAIL; n++, t = M32(t + T_TLNEXT)) {
        sim_hex("thr", t);
        sim_hex("id", M32(t + T_ID));
        sim_hex("pri", M32(t + T_PRI));
        sim_hex("st", M16(t + T_STATE));
        uint32_t q = M32(t + T_QUEUE);
        sim_hex("q", q);
        if (rdram_word(q) && q != RUNQ) {
            sim_hex("valid", M32(q + 8)), sim_hex("count", M32(q + 0x10));
        }
        const struct fiber *f = 0;
        for (int i = 0; i < NFIBER && !f; i++)
            if (fibers[i].thread == t) f = &fibers[i];
        if (f) {
            uint32_t at, best = f->tick ? fn_below(f->tick, &at) : 0;  // the function holding its last back-edge poll
            if (best) sim_hex("tick", best), sim_hex("+", f->tick - at);
            uint32_t sp = f->ctx.gpr[29];
            sim_hex("ra", f->ctx.gpr[31]);
            sim_hex("sp", sp);
            for (uint32_t a = sp, k = 0; a < sp + 0x400 && k < 8 && rdram_word(a); a += 4) {
                uint32_t w = M32(a);
                if (rdram_word(w - 8) && (M32(w - 8) >> 26) == 3) sim_hex("<", w), k++;
            }
            uint32_t chain[CHAIN_MAX], n_chain = host_chain(f, chain);
            for (uint32_t k = 0; k < n_chain; k++) sim_hex("h<", chain[k]);
        }
        sim_puts("\n");
    }
    for (uint32_t e = 0; e < 16 && OS_EVENT_TAB; e++)
        if (M32(OS_EVENT_TAB + 8 * e)) {
            sim_hex("event", e);
            sim_hex("q", M32(OS_EVENT_TAB + 8 * e));
            sim_hex("msg", M32(OS_EVENT_TAB + 8 * e + 4));
            uint32_t q = M32(OS_EVENT_TAB + 8 * e), valid = M32(q + 8), count = M32(q + 0x10);
            if (rdram_word(q) && valid && valid <= count && rdram_word(M32(q + 0x14))) {
                uint32_t m = M32(M32(q + 0x14) + 4 * (M32(q + 0xC) % count));
                sim_hex("queued", valid), sim_hex("first", m);
                char text[72];
                int n = 0;
                for (uint32_t a = m; rdram_word(m) && n < 71; a++, n++) {
                    char c = (char)*(volatile uint8_t *)((a ^ 3) + t_rdram_bias);
                    if (c < 0x20 && c != '\n') break;
                    text[n] = c == '\n' ? '|' : c;
                }
                text[n] = 0;
                if (n > 2) sim_puts(" \""), sim_puts(text), sim_puts("\"");
            }
            sim_puts("\n");
        }
}

// Thread stacks hold stale $ra spills, so this hashes the threads and the event table rather than all of RDRAM
uint32_t os_state_hash(void) {
    static const uint32_t thread_words[] = { T_NEXT, T_PRI, T_QUEUE, T_TLNEXT, T_STATE, T_ID, T_SR, T_RCP };
    uint32_t h = crc32_word(crc32_word(0, rd32(RUNQ)), rd32(RUNNING));
    uint32_t t = rd32(ACTIVEQ);
    for (int n = 0; n < 32 && t != OS_THREAD_TAIL; n++, t = rd32(t + T_TLNEXT))
        for (unsigned i = 0; i < sizeof thread_words / 4; i++) h = crc32_word(h, rd32(t + thread_words[i]));
    for (uint32_t e = 0; e < 16; e++) {
        uint32_t q = rd32(OS_EVENT_TAB + 8 * e);
        h = crc32_word(crc32_word(h, q), rd32(OS_EVENT_TAB + 8 * e + 4));
        if (q) h = crc32_word(crc32_word(crc32_word(h, rd32(q)), rd32(q + 8)), rd32(q + 0xC));
    }
    return h;
}

void os_dump(void) {
    os_report();
    dump_threads();
}

void os_die(const char *why, uint32_t v) {
    sim_puts(why);
    sim_hex(" ", v);
    sim_puts("\n");
    os_report();
    dump_threads();
    if (os_hooks.die) os_hooks.die();
    sim_exit(95);
    for (;;) {}
}

void os_advance(unsigned long long dt) {
    vclock += dt;
    if (cmp_armed && vclock >= cmp_at) {
        cmp_pending = 1;
        cmp_at += 1ull << 32;
    }
    rcp_tick(vclock);
}

static void set_compare(uint32_t v) {
    unsigned long long t = (vclock & ~0xFFFFFFFFull) | v;
    if (t <= vclock) t += 1ull << 32;
    c0_compare = v;
    cmp_at = t;
    cmp_pending = 0;
    cmp_armed = 1;
}

static uint32_t cause_ip(void) { return (rcp_mi_pending() ? 0x400u : 0) | (cmp_pending ? 0x8000u : 0); }

static void enqueue(uint32_t q, uint32_t t) {
    uint32_t prev = q, nx = M32(q);
    int32_t pri = (int32_t)M32(t + T_PRI);
    while ((int32_t)M32(nx + T_PRI) >= pri) {
        prev = nx;
        nx = M32(nx);
    }
    M32(t) = nx;
    M32(prev) = t;
    M32(t + T_QUEUE) = q;
}

static uint32_t pop(uint32_t q) {
    uint32_t t = M32(q);
    M32(q) = M32(t);
    return t;
}

static void send_event(uint32_t off) {
    uint32_t es = OS_EVENT_TAB + off, mq = M32(es);
    os_trace_hash = crc32_word(os_trace_hash, off);
    if (!mq) return;
    int32_t valid = (int32_t)M32(mq + 8), count = (int32_t)M32(mq + 0x10);
    if (valid >= count) return;
    uint32_t slot = (uint32_t)(((int32_t)M32(mq + 0xC) + valid) % count);
    M32(M32(mq + 0x14) + 4 * slot) = M32(es + 4);
    M32(mq + 8) = (uint32_t)(valid + 1);
    if (M32(M32(mq)) != 0) enqueue(RUNQ, pop(mq));
}

static int on_active_list(uint32_t t) {
    for (uint32_t a = M32(ACTIVEQ), n = 0; rdram_word(a) && a != OS_THREAD_TAIL && n < 4096; a = M32(a + T_TLNEXT), n++)
        if (a == t) return 1;
    return 0;
}

uint32_t os_fibers_peak;

static struct fiber *fiber_for(uint32_t t) {
    struct fiber *spare = 0;
    for (int i = 1; i < NFIBER; i++) {
        if (fibers[i].thread == t) return &fibers[i];
        if (!fibers[i].thread && !spare) spare = &fibers[i];
    }
    for (int i = 1; i < NFIBER && !spare; i++)  // osDestroyThread unlinked it, so its fiber is free
        if (&fibers[i] != cur && !on_active_list(fibers[i].thread)) spare = &fibers[i];
    if (!spare) os_die("os: out of fibers for thread", t);
    spare->thread = t;
    uint32_t used = 0;
    for (int i = 1; i < NFIBER; i++) used += fibers[i].thread != 0;
    if (used > os_fibers_peak) os_fibers_peak = used;
    return spare;
}

static void fiber_main(void);
static void dispatch(void);
static void exception(uint32_t ip, uint32_t run);

// A context fiber_switch starts at entry on the stack below top, with the current FPSCR
static void fiber_init(uint32_t regs[16], const uint32_t *top, void (*entry)(void)) {
    uint32_t fpscr;
    __asm__ volatile("sts fpscr,%0" : "=r"(fpscr));
    for (int i = 0; i < 16; i++) regs[i] = 0;
    regs[7] = (uint32_t)top;
    regs[8] = (uint32_t)entry;
    regs[11] = fpscr;
}

// The RSP's fiber: a task parks when an interrupt comes due, and the CPU resumes it in its own scheduling
static uint32_t cop_regs[16], cop_caller[16];
static void (*cop_body)(void);
static int cop_live;
static unsigned long long cop_deadline;

static void cop_main(void) {
    cop_body();
    cop_live = 0;
    fiber_switch(cop_regs, cop_caller);
    os_die("os: a finished RSP fiber was resumed", 0);
}

void os_cop_start(void (*body)(void)) {
    if (cop_live) os_die("os: RSP fiber started while one is live", 0);
    if (!stack_lo) os_die("os: no fiber stack pool for the RSP", 0);
    fiber_init(cop_regs, stack_lo + NFIBER * STACK_WORDS, cop_main);
    cop_body = body;
    cop_live = 1;
}

static unsigned long long next_due(void) {
    unsigned long long t = rcp_next_event(vclock);
    return cmp_armed && cmp_at < t ? cmp_at : t;
}

int os_cop_run(void) {
    cop_deadline = next_due();
    fiber_switch(cop_caller, cop_regs);
    return !cop_live;
}

int os_cop_due(void) { return clock_now() >= cop_deadline; }
void os_cop_park(void) { fiber_switch(cop_regs, cop_caller); }

// A parked graphics task outranks only the lowest non-idle priority, as the game loop's own work (the DC ports draw
// in that thread, while audio preempts it at each vblank)
static int below_rsp(uint32_t t) {
    int32_t pri = (int32_t)M32(t + T_PRI), floor = 0x7FFFFFFF;
    for (uint32_t a = M32(ACTIVEQ); a && a != OS_THREAD_TAIL; a = M32(a + T_TLNEXT)) {
        int32_t p = (int32_t)M32(a + T_PRI);
        if (p > 0 && p < floor) floor = p;
    }
    return pri <= floor;
}

static void idle_step(void) {
    if (rcp_rsp_idle()) {
        catch_up();
        return;
    }
    unsigned long long t = rcp_next_event(vclock);
    if (cmp_armed && cmp_at < t) t = cmp_at;
    if (t == RCP_NO_EVENT) os_die("os: deadlock, every thread is blocked and no interrupt is due", M32(RUNNING));
    clock_wait(t);
    catch_up();
    os_advance(t > vclock ? t - vclock : 0);
}

int os_rsp_outranks_running(void) {
    uint32_t run = M32(RUNNING);
    return !run || below_rsp(run);
}

void os_poll(void) {
    if ((c0_status & 3) != 1) return;
    uint32_t ip = cause_ip() & c0_status & 0xFF00;
    if (ip) exception(ip, M32(RUNNING));
}

static void handle_mi(void) {
    uint32_t s1 = rcp_rd(0x04300008, 4) & 0x3F;  // JP: every pending bit, masked or not
    if (s1 & MI_INTR_SP) {
        uint32_t st = rcp_rd(0x04040010, 4);
        rcp_wr(0x04040010, 0x8, 4);
        send_event(st & 0x300 ? 32 : 88);
    }
    if (s1 & MI_INTR_VI) { rcp_wr(0x04400010, 0, 4); send_event(56); }
    if (s1 & MI_INTR_AI) { rcp_wr(0x0450000C, 1, 4); send_event(48); }
    if (s1 & MI_INTR_SI) { rcp_wr(0x04800018, 0, 4); send_event(40); }
    if (s1 & MI_INTR_PI) { rcp_wr(0x04600010, 2, 4); send_event(64); }
    if (s1 & MI_INTR_DP) { rcp_wr(0x04300000, 0x800, 4); send_event(72); }
}

static void exception(uint32_t ip, uint32_t run) {
    uint32_t sr = c0_status;
    c0_status |= 2;
    if (run) {
        M32(run + T_SR) = sr | 2;
        M32(run + T_RCP) = rcp_mi_mask();
        M16(run + T_STATE) = ST_RUNNABLE;
    }
    if (ip & 0x8000) {
        set_compare(c0_compare);
        send_event(24);
    }
    if (ip & 0x400) handle_mi();
    if (!run) {
        c0_status = sr;
        return;
    }
    if ((int32_t)M32(run + T_PRI) < (int32_t)M32(M32(RUNQ) + T_PRI) || (rcp_rsp_parked() && below_rsp(run))) {
        M32(run + T_PC) = SUSPENDED;
        enqueue(RUNQ, run);
        dispatch();
        return;
    }
    M16(run + T_STATE) = ST_RUNNING;
    c0_status = sr;
}

static void dispatch(void) {
    uint32_t t;
    while ((t = M32(RUNQ)) == OS_THREAD_TAIL || (rcp_rsp_parked() && below_rsp(t))) {
        M32(RUNNING) = 0;
        idle_step();
        uint32_t ip = cause_ip() & 0xFF00;
        if (ip) exception(ip, 0);
    }
    pop(RUNQ);
    M32(RUNNING) = t;
    M16(t + T_STATE) = ST_RUNNING;
    rcp_wr(MI_INTR_MASK_REG, M16(OS_RCP_IM_TABLE + 2 * (M32(t + T_RCP) & 0x3F)), 4);
    c0_status = M32(t + T_SR) & ~2u;
    os_switches++;
    os_trace_hash = crc32_word(crc32_word(os_trace_hash, M32(t + T_ID)), (uint32_t)vclock);
    struct fiber *f = fiber_for(t);
    if (M32(t + T_PC) != SUSPENDED) {
        if (f == cur) os_die("os: a running thread was re-created under itself", t);
        if (!stack_lo) os_die("os: no fiber stack pool; KOS would reject the stack", t);
        fiber_init(f->regs, stack_lo + (f - fibers) * STACK_WORDS, fiber_main);
        f->depth = 0;
    }
    if (f != cur) {
        struct fiber *from = cur;
        cur = f;
        fiber_switch(from->regs, f->regs);
    }
    struct fiber *me = cur;
    if (++me->depth > 32) os_die("os: interrupt nesting runaway in thread", M32(RUNNING));
    os_poll();
    me->depth--;
}

static void fiber_main(void) {
    struct fiber *f = cur;
    uint32_t t = f->thread, pc = M32(t + T_PC);
    struct tctx *c = &f->ctx;
    for (unsigned i = 0; i < sizeof *c / 4; i++) ((uint32_t *)c)[i] = 0;
    c->gpr[4] = M32(t + T_A0);
    c->gpr[29] = M32(t + T_SP);
    c->gpr[31] = M32(t + T_RA);
    c->fcsr = M32(t + T_FPCSR);  // osCreateThread's FPCSR_FS | FPCSR_EV
    os_poll();
    t_call((void (*)(void))t_lookup(pc), c);
    if (!OS_DESTROY_THREAD) os_die("os: thread returned, and this ROM's osDestroyThread is unidentified", t);
    c->gpr[4] = 0;
    t_call((void (*)(void))t_lookup(OS_DESTROY_THREAD), c);
    os_die("os: osDestroyThread(NULL) returned in thread", t);
}

void os_init(uint32_t boot_sp) { fibers[0].ctx.gpr[29] = boot_sp; }

void os_set_stack_pool(void *lo, uint32_t bytes) {
    if (bytes < OS_STACK_POOL_BYTES || ((uint32_t)lo & 7)) os_die("os: fiber stack pool too small or misaligned", bytes);
    stack_lo = lo;
    if (bytes >= OS_STACK_POOL_BYTES + OS_BIG_STACK_BYTES)
        big_top = stack_lo + OS_BIG_STACK_BYTES / 4, stack_lo = big_top;
    memset(stack_lo, 0, OS_STACK_POOL_BYTES);  // os_report's high-water marks
}

static uint32_t big_regs[16], big_caller[16];
static void (*big_fn)(uint32_t);
static uint32_t big_arg;

static void big_main(void) {
    big_fn(big_arg);
    fiber_switch(big_regs, big_caller);
    os_die("os: a finished deep call was resumed", 0);
}

void os_big_call(void (*fn)(uint32_t), uint32_t arg) {
    if (big_fn) os_die("os: a deep call inside a deep call", 0);
    if (!big_top) os_die("os: the stack pool has no room for deep calls", 0);
    fiber_init(big_regs, big_top, big_main);
    big_fn = fn, big_arg = arg;
    fiber_switch(big_caller, big_regs);
    big_fn = 0;
}

uint32_t os_call(uint32_t vram, const uint32_t a[8]) {
    struct tctx *c = &cur->ctx;
    uint32_t sp = c->gpr[29];
    c->gpr[29] = sp - 32;
    for (int i = 0; i < 4; i++) {
        c->gpr[4 + i] = a[i];
        M32(sp - 16 + 4 * i) = a[4 + i];
    }
    t_call((void (*)(void))t_lookup(vram), c);
    c->gpr[29] = sp;
    return c->gpr[2];
}

// Loop back-edges in translated code count down t_budget; at zero, compute has taken virtual time and interrupts land
uint32_t t_budget = OS_TICK_EDGES;

#define RSP_STARVE_COUNTS 94000ull  // ~2 ms parked while threads above the floor keep the CPU (46.875 MHz Count)
static unsigned long long rsp_parked_since;

// The RSP runs beside the CPU: a parked graphics task gets a slice even while a busy thread outranks it (Pokemon
// Stadium 2's loader polls for that very task's completion)
static void rsp_share(void) {
    if (!rcp_rsp_parked()) {
        rsp_parked_since = 0;
        return;
    }
    if (!rsp_parked_since) {
        rsp_parked_since = vclock | 1;
        return;
    }
    if (vclock - rsp_parked_since < RSP_STARVE_COUNTS) return;
    rsp_parked_since = 0;
    rcp_rsp_idle();
}

void t_tick(struct tctx *c) {
    (void)c;
    cur->tick = (uint32_t)(uintptr_t)__builtin_return_address(0);
    t_budget = OS_TICK_EDGES;
    catch_up();
    rsp_share();
    os_poll();
}

void t_idle(struct tctx *c) {
    (void)c;
    idle_step();
    os_poll();
}

void h_os_enqueue_and_yield(struct tctx *c) {
    uint32_t run = M32(RUNNING);
    M32(run + T_SR) = c0_status | 2;
    M32(run + T_RCP) = rcp_mi_mask();
    M32(run + T_PC) = SUSPENDED;
    if (c->gpr[4]) enqueue(c->gpr[4], run);
    dispatch();
}

void h_os_dispatch(struct tctx *c) {
    (void)c;
    dispatch();
}

void h_send_mesg(struct tctx *c) { send_event(c->gpr[4]); }

void h_os_set_int_mask(struct tctx *c) {
    uint32_t im = c->gpr[4];
    c->gpr[2] = (c0_status & 0xFF01) | rcp_mi_mask() << 16;
    rcp_wr(MI_INTR_MASK_REG, M16(OS_RCP_IM_TABLE + ((im & 0x3F0000) >> 15)), 4);
    c0_status = (c0_status & 0xFFFF00FFu) | (im & 0xFF01);
    os_poll();
}

void h_cop0_disable_int(struct tctx *c) { c->gpr[2] = c0_status & 1; c0_status &= ~1u; }
void h_cop0_restore_int(struct tctx *c) { c0_status |= c->gpr[4]; os_poll(); }
void h_cop0_get_sr(struct tctx *c) { c->gpr[2] = c0_status; }
void h_cop0_set_sr(struct tctx *c) { c0_status = c->gpr[4]; os_poll(); }
void h_cop0_get_cause(struct tctx *c) { c->gpr[2] = cause_ip(); }
void h_cop0_set_compare(struct tctx *c) { set_compare(c->gpr[4]); }

// The guest TLB as libultra's osMapTLB/osUnmapTLBAll write it; the current ASID is taken as 0
#define VTLB_N 32
#define LO_G 1u
#define LO_V 2u
#define LO_D 4u
#define HI_UNMAPPED 0x80000000u
struct vtlb { uint32_t mask, hi, lo[2]; };
static struct vtlb vtlb[VTLB_N];

static uint32_t vtlb_vmask(const struct vtlb *e) { return e->mask | 0x1FFFu; }
static int vtlb_live(const struct vtlb *e) { return (e->lo[0] & e->lo[1] & LO_G) || !(e->hi & 0xFFu); }

static void vtlb_sync(void) {
    hostmmu_begin();
    for (int i = 0; i < VTLB_N; i++) {
        const struct vtlb *e = &vtlb[i];
        uint32_t ps = (vtlb_vmask(e) + 1) >> 1, base = e->hi & ~vtlb_vmask(e);
        if (!vtlb_live(e) || (base >= 0x80000000u && base < 0xC0000000u)) continue;
        for (int k = 0; k < 2; k++) {
            uint32_t lo = e->lo[k], pa = (lo & 0x3FFFFFC0u) << 6;
            if (!(lo & LO_V)) continue;
            if (pa + ps > RDRAM_HI - RDRAM_LO) os_die("os: TLB page outside RDRAM", pa);
            hostmmu_page(base + k * ps + t_rdram_bias, (RDRAM_LO + pa + t_rdram_bias) & 0x1FFFFFFFu, ps, !!(lo & LO_D),
                         ((lo >> 3) & 7) != 2);
        }
    }
    hostmmu_end();
}

static uint32_t entry_lo(uint32_t paddr, uint32_t flags) { return paddr == 0xFFFFFFFFu ? LO_G : ((paddr >> 6) | flags) & 0x3FFFFFFFu; }

void h_os_map_tlb(struct tctx *c) {
    uint32_t even = c->gpr[7], odd = M32(c->gpr[29] + 16), asid = M32(c->gpr[29] + 20);
    struct vtlb *e = &vtlb[c->gpr[4] & (VTLB_N - 1)];
    uint32_t flags = asid == 0xFFFFFFFFu ? 31u : 30u;
    e->mask = c->gpr[5] & 0x01FFE000u;
    e->hi = (c->gpr[6] | (asid == 0xFFFFFFFFu ? 0 : asid)) & ~0x1F00u;
    e->lo[0] = entry_lo(even, flags);
    e->lo[1] = entry_lo(odd, flags);
    if (even == 0xFFFFFFFFu && odd == 0xFFFFFFFFu) e->hi = HI_UNMAPPED;
    vtlb_sync();
}

void h_os_unmap_tlb_all(struct tctx *c) {
    (void)c;
    for (int i = VTLB_N - 1; i > 0; i--) {  // as libultra's loop: entry 0 survives
        vtlb[i].hi = HI_UNMAPPED;
        vtlb[i].lo[0] = vtlb[i].lo[1] = 0;
    }
    vtlb_sync();
}

void h_os_inval_icache(struct tctx *c) {
    if (os_hooks.icache_inval && (int32_t)c->gpr[5] > 0) os_hooks.icache_inval(c->gpr[4], c->gpr[5]);
}

void h_cop0_probe_tlb(struct tctx *c) {
    uint32_t va = c->gpr[4];
    c->gpr[2] = 0xFFFFFFFFu;
    for (int i = 0; i < VTLB_N; i++) {
        const struct vtlb *e = &vtlb[i];
        if (((e->hi ^ va) & ~vtlb_vmask(e)) || !vtlb_live(e)) continue;
        uint32_t ps = (vtlb_vmask(e) + 1) >> 1, lo = e->lo[(va & ps) != 0];
        if (lo & LO_V) c->gpr[2] = ((lo & 0x3FFFFFC0u) << 6) + (va & (ps - 1));
        return;
    }
}

static uint32_t count_read(void) {
    catch_up();
    return (uint32_t)vclock;
}

void h_cop0_get_count(struct tctx *c) {
    c->gpr[2] = count_read();
    os_poll();
}

// mfc0/mtc0 in code no hook covers, on the hooks' COP0; the watch registers trap nothing here
static uint32_t c0_watch[2];

uint32_t os_mfc0(uint32_t r) {
    uint32_t v;
    switch (r) {
    case 9: v = count_read(); break;
    case 11: v = c0_compare; break;
    case 12: v = c0_status; break;
    case 13: v = cause_ip(); break;
    case 18: case 19: v = c0_watch[r - 18]; break;
    default: os_die("cop0: mfc0 of register", r);
    }
    os_poll();
    return v;
}

void os_mtc0(uint32_t r, uint32_t v) {
    switch (r) {
    case 11: set_compare(v); break;
    case 12: c0_status = v; break;
    case 18: case 19: c0_watch[r - 18] = v; break;
    default: os_die("cop0: mtc0 to register", r);
    }
    os_poll();
}
