#include <kos.h>
#include <arch/cache.h>
#include <string.h>

#include "dc64_restart.h"
#include "dc_exit.h"

#define P2(p) ((void *)(((uintptr_t)(p) & 0x1fffffffu) | 0xa0000000u))

extern uint8_t dc64_data_lo[], edata[], dc64_pristine[], dc64_pristine_end[];
extern uint32_t dc64_boots;
extern char dc64_note[48];
void dc64_reenter(void) __attribute__((noreturn));
void __real_arch_real_exit(int ret) __attribute__((noreturn));

static volatile bool g_armed;

void dc64_restart_arm(void) { g_armed = true; }

void dc64_restart_note(const char *note) {
    volatile char *p = P2(dc64_note);
    size_t i = 0;
    for (; note[i] && i < sizeof dc64_note - 1; i++) p[i] = note[i];
    p[i] = 0;
}

void dc64_take_note(char *out, size_t n) {
    volatile char *p = P2(dc64_note);
    size_t i = 0;
    for (; n && p[i] && i < n - 1 && i < sizeof dc64_note - 1; i++) out[i] = p[i];
    if (n) out[i] = 0;
    p[0] = 0;
}

bool dc64_restart_ready(void) { return edata - dc64_data_lo <= dc64_pristine_end - dc64_pristine; }

uint32_t dc64_boot_count(void) { return *(volatile uint32_t *)P2(&dc64_boots); }


// arch_abort's last call, after its own teardown and with IRQs off
void __wrap_arch_real_exit(int ret) {
    if (!g_armed || !dc64_restart_ready()) __real_arch_real_exit(ret);
    *(volatile uint32_t *)P2(&dc64_boots) += 1;
    memcpy(dc64_data_lo, P2(dc64_pristine), (size_t)(edata - dc64_data_lo));
    dcache_flush_all();
    dc64_reenter();
}
