// Its own thread polls each pad on its own Maple frame and ends the run with arch_abort.
#include <kos.h>
#include <arch/irq.h>
#include <arch/mmu.h>
#include <arch/timer.h>
#include <dc/maple.h>
#include <stdio.h>

#include "dc_exit.h"

#ifndef DC_DEADLINE_S
#define DC_DEADLINE_S 60
#endif
#if DC_DEADLINE_S <= 0
#error "DC_DEADLINE_S must be a real deadline"
#endif

#define EXIT_COMBO (CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START)
#define POLL_MS    5  // under a vblank, so port A's frame is back in the queue for every Maple flush
#define STUCK_MS   300

struct ask {
    maple_frame_t frame;
    uint32_t arg;
    uint64_t at;
};

static uint64_t g_deadline;
static struct ask g_pad_ask, g_other_ask;  // port A every vblank; B-D in turn, for the combo only
static volatile bool g_combo;
static volatile int g_combo_port;
static volatile uint32_t g_unreleased;  // ports whose combo still counts as the one that restarted the program
static volatile uint32_t g_pad = 0x80800000u;  // port A: buttons, then joyx << 16, joyy << 24 (128 = centered)
static volatile uint16_t g_trig;  // port A: rtrig, ltrig << 8

// IRQ context.
static void cond_reply(maple_state_t *st, maple_frame_t *frm) {
    (void)st;
    const maple_response_t *resp = (const maple_response_t *)frm->recv_buf;
    const uint32_t *d = (const uint32_t *)resp->data;
    if (resp->response == MAPLE_RESPONSE_DATATRF && resp->data_len >= 2 && d[0] == MAPLE_FUNC_CONTROLLER) {
        uint32_t buttons = ~*(const uint16_t *)(d + 1) & 0xffff;  // KOS cont_cond_t.buttons, active low
        const uint8_t *c = (const uint8_t *)(d + 1);
        uint32_t bit = 1u << frm->dst_port;
        if ((buttons & EXIT_COMBO) != EXIT_COMBO) g_unreleased &= ~bit;
        else if (!(g_unreleased & bit)) g_combo = true, g_combo_port = frm->dst_port;
        if (frm->dst_port == 0) g_pad = buttons | (uint32_t)c[4] << 16 | (uint32_t)c[5] << 24, g_trig = c[2] | c[3] << 8;
    }
    maple_frame_unlock(frm);
}

// Returns 1 once a new request is queued; a pad that never answers has its frame taken back after STUCK_MS.
static int ask(struct ask *a, int port, uint64_t now) {
    maple_frame_t *f = &a->frame;
    if (f->state != MAPLE_FRAME_VACANT) {
        if (now - a->at > STUCK_MS) {
            uint32_t old = irq_disable();
            if (f->queued) maple_queue_remove(f);
            f->state = MAPLE_FRAME_VACANT;
            irq_restore(old);
        }
        return 0;
    }
    if (maple_frame_lock(f) < 0) return 0;
    maple_frame_init(f);
    a->arg = MAPLE_FUNC_CONTROLLER;
    a->at = now;
    f->cmd = MAPLE_COMMAND_GETCOND;
    f->dst_port = port;
    f->dst_unit = 0;
    f->length = 1;
    f->send_buf = &a->arg;
    f->callback = cond_reply;
    maple_queue_frame(f);
    return 1;
}

static void *exit_thread(void *arg) {
    (void)arg;
    int port = 1;
    for (;;) {
        uint64_t now = timer_ms_gettime64();
        if (g_combo) {
            printf("exit: A+B+X+Y+Start on port %c\n", 'A' + g_combo_port);
            if (dc_exit_on_combo) dc_exit_on_combo();
            fflush(stdout);
            break;
        }
        if (now >= g_deadline) {
            printf("exit: deadline %u s\n", (unsigned)DC_DEADLINE_S);
            if (dc_exit_on_deadline) dc_exit_on_deadline();
            fflush(stdout);
            break;
        }
        ask(&g_pad_ask, 0, now);
        if (ask(&g_other_ask, port, now)) port = port % (MAPLE_PORT_COUNT - 1) + 1;
        thd_sleep(POLL_MS);
    }
    arch_abort();
    return NULL;
}

void (*dc_exit_on_deadline)(void);
void (*dc_exit_on_combo)(void);

#ifdef DC_EXIT_RESTARTS
#include "dc64_restart.h"
static bool restarted(void) { return dc64_boot_count() != 0; }
#else
static bool restarted(void) { return false; }  // only the menu ELFs restart, through dc64_restart.c
#endif

uint32_t dc_exit_pad(void) { return g_pad; }
uint32_t dc_exit_triggers(void) { return g_trig; }

bool dc_exit_arm(void) {
    static bool armed;
    if (armed) return true;
    armed = true;
    g_unreleased = restarted() ? 0xFu : 0;  // each port's combo counts only once let go, after the restart it made
    g_deadline = timer_ms_gettime64() + DC_DEADLINE_S * 1000ull;
    g_pad_ask.frame.state = g_other_ask.frame.state = MAPLE_FRAME_VACANT;
    kthread_attr_t attr = { 0 };
    attr.create_detached = 1;
    attr.stack_size = 8192;
    attr.prio = PRIO_DEFAULT - 2;
    attr.label = "dc_exit";
    if (thd_create_ex(&attr, exit_thread, NULL)) return true;
    printf("exit: thread failed to start\n");
    return false;
}

void __real_arch_abort(void) __attribute__((noreturn));
void __real_arch_exit(void) __attribute__((noreturn));

// --wrap: dcmmu_init leaves MMUCR.AT on, and a dcload before 65a73ab TLB-misses on it and reruns the program forever
void __wrap_arch_abort(void) {
    mmu_shutdown_basic();
    __real_arch_abort();
}

void __wrap_arch_exit(void) {
    mmu_shutdown_basic();
    __real_arch_exit();
}

#define SCAN_WAIT_MS 3000

void __real_maple_wait_scan(void);

// Linked over KOS init's maple_wait_scan (--wrap), which spins forever on a port whose scan never finishes.
void __wrap_maple_wait_scan(void) {
    if (!dc_exit_arm()) arch_abort();
    uint64_t give_up = timer_ms_gettime64() + SCAN_WAIT_MS;
    while (maple_state.scan_ready_mask != 0xf) {
        if (timer_ms_gettime64() >= give_up) {
            printf("maple: scan unfinished after %u ms (ready mask %x), going on\n", SCAN_WAIT_MS,
                   maple_state.scan_ready_mask);
            return;
        }
        thd_sleep(10);
    }
    __real_maple_wait_scan();
}
