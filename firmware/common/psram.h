// The PGA2350's 8 MB QSPI PSRAM (APS6404L) on QMI chip select 1, GP47, mapped at PSRAM_BASE. Core 0.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "console.h"

#define PSRAM_BASE   0x11000000u        // cached XIP window; writes through it need XIP_CTRL.WRITABLE_M1
#define PSRAM_CS_PIN 47

uint32_t psram_init(void);              // bytes found, 0 if no PSRAM answered its ID read
uint32_t psram_size(void);

// Flash erase and program take the QMI from both windows, so core 1 must not touch PSRAM meanwhile: core 0 wraps
// every flash operation in psram_lock / psram_unlock, core 1 every PSRAM access in psram_enter / psram_leave.
void psram_lock(void);
void psram_unlock(void);
bool psram_enter(void);                 // false: a flash operation holds the QMI; answer NOT_READY
void psram_leave(void);

extern const console_cmd_t psram_commands[];
extern const unsigned psram_ncommands;
