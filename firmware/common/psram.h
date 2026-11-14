// The PGA2350's 8 MB QSPI PSRAM (APS6404L) on QMI chip select 1, GP47, mapped at PSRAM_BASE. Core 0.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PSRAM_BASE   0x11000000u        // cached XIP window; writes through it need XIP_CTRL.WRITABLE_M1
#define PSRAM_CS_PIN 47

// Who owns which bytes
#define PSRAM_PASS_STARTS 0x00000000u   // N64 whole-ROM pass: its jr ra starts, 48 KB
#define PSRAM_XC_INDEX    0x00010000u   // SD translation cache: the card's index, mirrored, 128 KB
#define PSRAM_XC_STAGE    0x00040000u   // SD translation cache: the entry in flight, to the end of PSRAM

uint32_t psram_init(void);              // bytes found, 0 if no PSRAM answered its ID read
uint32_t psram_size(void);

// Flash erase and program take the QMI from both windows, so core 1 must not touch PSRAM meanwhile: core 0 wraps
// every flash operation in psram_lock / psram_unlock, core 1 every PSRAM access in psram_enter / psram_leave.
void psram_lock(void);
void psram_unlock(void);
bool psram_enter(void);                 // false: a flash operation holds the QMI; answer NOT_READY
void psram_leave(void);
