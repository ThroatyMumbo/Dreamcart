#pragma once

// The core voltage and clock the Maple link needs, the UART console, then any fault the last boot left.
void boot_init(void);

// boot_reset.S armed the watchdog to catch a lockup on bad XIP reads; call once init is past that.
void boot_done(void);
