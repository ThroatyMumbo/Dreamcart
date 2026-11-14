// Replaces pico_crt0's .reset section (PICO_CRT0_NO_RESET_SECTION=1); vectors, binary info and runtime_init stay SDK.
#include "pico.h"
#include "pico/asm_helper.S"
#include "pico/platform/cpu_regs.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/sio.h"
#include "hardware/regs/psm.h"
#include "hardware/regs/ticks.h"
#include "hardware/regs/watchdog.h"
#include "boot/bootrom_constants.h"

#if PICO_EMBED_XIP_SETUP || PICO_NO_FLASH || !PICO_COPY_TO_RAM
#error "boot_reset.S is written for a copy_to_ram flash image with no embedded XIP setup"
#endif

pico_default_asm_setup

#define BOOT_WATCHDOG_TICKS 4000000
#define XIP_NOCACHE_OFFSET  (XIP_NOCACHE_NOALLOC_BASE - XIP_BASE)
#define CLEAN_PASSES        2
#define WDSEL_ALL_BUT_OSCS  (PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS))
#define WD_CTRL_ARMED       (WATCHDOG_CTRL_ENABLE_BITS | WATCHDOG_CTRL_PAUSE_DBG0_BITS | \
                             WATCHDOG_CTRL_PAUSE_DBG1_BITS | WATCHDOG_CTRL_PAUSE_JTAG_BITS)
#define WATCHDOG_NON_REBOOT_MAGIC 0x6ab73121

.section .reset, "ax"

.type _entry_point,%function
.thumb_func
.global _entry_point
_entry_point:
    ldr r0, =BOOTROM_VTABLE_OFFSET

_enter_vtable_in_r0:
    ldr r1, =(PPB_BASE + ARM_CPU_PREFIXED(VTOR_OFFSET))
    str r0, [r1]
    ldmia r0!, {r1, r2}
    msr msp, r1
    bx r2

.type _reset_handler,%function
.thumb_func
.global _reset_handler
_reset_handler:
    ldr r0, =(SIO_BASE + SIO_CPUID_OFFSET)
    ldr r0, [r0]
    cbz r0, 1f
hold_non_core0_in_bootrom:
    ldr r0, =BOOTROM_VTABLE_OFFSET
    b _enter_vtable_in_r0
1:
    // armed before the first flash read, so a lockup on a bad XIP fetch reboots instead of hanging
    ldr r0, =TICKS_BASE
    movs r1, #12
    str r1, [r0, #TICKS_WATCHDOG_CYCLES_OFFSET]
    movs r1, #TICKS_WATCHDOG_CTRL_ENABLE_BITS
    str r1, [r0, #TICKS_WATCHDOG_CTRL_OFFSET]
    ldr r0, =PSM_BASE
    ldr r1, =WDSEL_ALL_BUT_OSCS
    str r1, [r0, #PSM_WDSEL_OFFSET]
    ldr r0, =WATCHDOG_BASE
    ldr r1, =WATCHDOG_NON_REBOOT_MAGIC
    str r1, [r0, #WATCHDOG_SCRATCH4_OFFSET]
    ldr r1, =BOOT_WATCHDOG_TICKS
    str r1, [r0, #WATCHDOG_LOAD_OFFSET]
    ldr r1, =WD_CTRL_ARMED
    str r1, [r0, #WATCHDOG_CTRL_OFFSET]

    // copy through the uncached alias so a bad read never lands in a cache line we execute from
copy_image:
    adr r4, data_cpy_table
    add r4, r4, #XIP_NOCACHE_OFFSET
2:
    ldmia r4!, {r1-r3}
    cbz r1, 3f
    add r1, r1, #XIP_NOCACHE_OFFSET
    bl data_cpy
    b 2b
3:
    movs r5, #CLEAN_PASSES
verify_pass:
    adr r4, data_cpy_table
    add r4, r4, #XIP_NOCACHE_OFFSET
4:
    ldmia r4!, {r1-r3}
    cbz r1, 5f
    add r1, r1, #XIP_NOCACHE_OFFSET
    bl data_cmp
    cmp r0, #0
    bne copy_image
    b 4b
5:
    subs r5, #1
    bne verify_pass

    ldr r1, =__bss_start__
    ldr r2, =__bss_end__
    movs r0, #0
    b bss_fill_test
bss_fill_loop:
    stm r1!, {r0}
bss_fill_test:
    cmp r1, r2
    bne bss_fill_loop

    ldr r1, =boot_from_ram
    bx r1

data_cpy_loop:
    ldm r1!, {r0}
    stm r2!, {r0}
data_cpy:
    cmp r2, r3
    blo data_cpy_loop
    bx lr

// r0 = 0 when [r2, r3) matches the flash at r1
data_cmp:
    movs r0, #0
1:
    cmp r2, r3
    bhs 2f
    ldr r6, [r1], #4
    ldr r7, [r2], #4
    cmp r6, r7
    beq 1b
    movs r0, #1
2:
    bx lr

.p2align 2
.global data_cpy_table
data_cpy_table:
.word __ram_text_source__
.word __ram_text_start__
.word __ram_text_end__

.word __etext
.word __data_start__
.word __data_end__

.word __scratch_x_source__
.word __scratch_x_start__
.word __scratch_x_end__

.word __scratch_y_source__
.word __scratch_y_start__
.word __scratch_y_end__

.word 0

.ltorg

// runs from the verified RAM copy; drops every XIP line fetched while flash reads were bad
.section .text.boot_from_ram, "ax"
.type boot_from_ram,%function
.thumb_func
boot_from_ram:
    ldr r0, =(XIP_MAINTENANCE_BASE + XIP_NOCACHE_OFFSET - 0x4000)
    ldr r1, =(XIP_MAINTENANCE_BASE + XIP_NOCACHE_OFFSET)
    movs r2, #0
1:
    strb r2, [r0], #8
    cmp r0, r1
    blo 1b
    dsb
    isb
    ldr r1, =runtime_init
    blx r1
    ldr r1, =main
    blx r1
    ldr r1, =exit
    blx r1
2:
    bkpt #0
    b 2b
