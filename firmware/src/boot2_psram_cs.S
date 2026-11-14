#include "hardware/regs/addressmap.h"
#include "hardware/regs/pads_bank0.h"
#include "hardware/regs/io_bank0.h"
#include "hardware/regs/sio.h"

#define PSRAM_CS_PIN 47
#define PSRAM_CS_HI_BIT (1u << (PSRAM_CS_PIN - 32))
#define PSRAM_CS_PAD (PADS_BANK0_GPIO0_IE_BITS | PADS_BANK0_GPIO0_PUE_BITS | \
                      PADS_BANK0_GPIO0_SCHMITT_BITS | (1u << PADS_BANK0_GPIO0_DRIVE_LSB))

.syntax unified
.cpu cortex-m33
.thumb
.section .text

// crt0 enters stage 2 at offset 0, so this must assemble ahead of the SDK code it tail-calls
// a PSRAM that powers up late on a slow supply latches bus traffic as a read while CS1 sits on the pad pull-down
.global _stage2_boot_psram_cs
.type _stage2_boot_psram_cs,%function
.thumb_func
_stage2_boot_psram_cs:
    ldr r3, =PADS_BANK0_BASE + PADS_BANK0_GPIO0_OFFSET + 4 * PSRAM_CS_PIN
    movs r0, #PSRAM_CS_PAD
    str r0, [r3]
    ldr r3, =SIO_BASE
    ldr r0, =PSRAM_CS_HI_BIT
    str r0, [r3, #SIO_GPIO_HI_OUT_SET_OFFSET]
    str r0, [r3, #SIO_GPIO_HI_OE_SET_OFFSET]
    ldr r3, =IO_BANK0_BASE + IO_BANK0_GPIO47_CTRL_OFFSET
    movs r0, #IO_BANK0_GPIO47_CTRL_FUNCSEL_VALUE_SIOB_PROC_47
    str r0, [r3]
    b _stage2_boot
.ltorg

#include "boot2_w25q080.S"
