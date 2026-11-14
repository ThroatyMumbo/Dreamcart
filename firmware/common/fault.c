#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "fault.h"

#define SCB_CFSR (*(volatile uint32_t *)0xe000ed28u)
#define SCB_HFSR (*(volatile uint32_t *)0xe000ed2cu)
#define SCB_BFAR (*(volatile uint32_t *)0xe000ed38u)
#define CFSR_BFARVALID (1u << 15)
#define FAULT_MAGIC 0x0FA017EDu

// watchdog scratch survives a watchdog reset, so the fault is reported on the next boot
__attribute__((used)) void fault_capture(uint32_t *frame) {
    watchdog_hw->scratch[1] = frame[6];
    watchdog_hw->scratch[2] = frame[5];
    watchdog_hw->scratch[3] = SCB_CFSR;
    watchdog_hw->scratch[7] = SCB_HFSR;     // not 4: watchdog_reboot zeroes it and boot_reset.S rewrites it
    watchdog_hw->scratch[5] = SCB_BFAR;
    watchdog_hw->scratch[0] = FAULT_MAGIC;
    watchdog_reboot(0, 0, 10);
    for (;;) tight_loop_contents();
}

__attribute__((naked)) void isr_hardfault(void) {
    __asm__ volatile(
        "mov  r0, lr        \n"
        "movs r1, #4        \n"
        "tst  r0, r1        \n"
        "bne  1f            \n"
        "mrs  r0, msp       \n"
        "b    2f            \n"
        "1:                 \n"
        "mrs  r0, psp       \n"
        "2:                 \n"
        "b    fault_capture \n");
}

void fault_report(void) {
    if (watchdog_hw->scratch[0] != FAULT_MAGIC) return;
    printf("\n*** previous boot faulted ***\n");
    printf("  PC   %08lX\n", (unsigned long)watchdog_hw->scratch[1]);
    printf("  LR   %08lX\n", (unsigned long)watchdog_hw->scratch[2]);
    printf("  CFSR %08lX\n", (unsigned long)watchdog_hw->scratch[3]);
    printf("  HFSR %08lX\n", (unsigned long)watchdog_hw->scratch[7]);
    printf("  BFAR %08lX%s\n", (unsigned long)watchdog_hw->scratch[5],
           (watchdog_hw->scratch[3] & CFSR_BFARVALID) ? "" : "  (not valid: CFSR.BFARVALID clear)");
    watchdog_hw->scratch[0] = 0;
}
