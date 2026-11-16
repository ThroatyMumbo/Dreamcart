#ifndef OS_H
#define OS_H

#include "htypes.h"

#define OS_FIBER_STACK_BYTES 4096u  // Kirby 64, 150 s: 844 bytes the deepest fiber, 1656 the RSP's
#define OS_NFIBER 48u  // fiber 0 is the caller's own; Kirby 64's object threads peak at 30 live
#define OS_STACK_POOL_BYTES (OS_NFIBER * OS_FIBER_STACK_BYTES)  // fibers 1..OS_NFIBER-1, then the RSP's
#define OS_BIG_STACK_BYTES 0x40000u  // os_big_call's, below the fibers when the pool has room: xlate recurses deep

#define OS_COUNT_HZ 46875000ull  // the CPU's Count register
// Translated loops poll every OS_TICK_EDGES back-edges; each tick reads the console clock (~1.5 us), so 256 is tens of us
#define OS_TICK_EDGES 256u

void os_init(uint32_t boot_sp);
void os_set_stack_pool(void *lo, uint32_t bytes);  // KOS checks sp against the running thread's stack base
void os_big_call(void (*fn)(uint32_t arg), uint32_t arg);  // fn on the big stack, from whichever fiber is running
uint32_t os_call(uint32_t vram, const uint32_t a[8]);  // a0-a3, then the stack words at 16..28(sp)
void os_die(const char *why, uint32_t v) __attribute__((noreturn));
// os_die's report and thread dump, without dying
void os_dump(void);
void os_report(void);
uint32_t os_state_hash(void);  // a boot's result: its threads and event table
// Entry i is guest[i * stride] at host[i * stride]: the guest function whose host start is the highest <= h (0: none)
uint32_t os_fn_below(const uint32_t *guest, const uint32_t *host, uint32_t stride, uint32_t n, uint32_t h, uint32_t *at);

unsigned long long os_now(void);
void os_advance(unsigned long long dt);
void os_poll(void);
uint32_t os_mfc0(uint32_t r);
void os_mtc0(uint32_t r, uint32_t v);
// Count follows now() (in counts) and idle waits for the next event; set before os_driver_main
void os_set_clock(unsigned long long (*now)(void), void (*wait)(unsigned long long until));

// The RSP's fiber. run returns 1 once body has returned, 0 when it parked; due: an interrupt the CPU should take
void os_cop_start(void (*body)(void));
int os_cop_run(void);
int os_cop_due(void);
void os_cop_park(void);
int os_rsp_outranks_running(void);  // the running thread is at the lowest non-idle priority, or there is none

extern uint32_t os_switches, os_trace_hash;
// What the OS HLE tells the boot's driver and the run-time code paths; NULL: nobody
struct os_hooks {
    void (*icache_inval)(uint32_t vaddr, uint32_t bytes);  // osInvalICache: code there may have changed
    void (*die)(void);
};
extern struct os_hooks os_hooks;

#endif
