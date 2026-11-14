#pragma once
#include <stdint.h>

// Console bring-up tests on the AD bus, each naming a fault by the cart edge pin to probe.
void cart_diag_open_lines(void);            // 'o'
void cart_diag_stuck(uint32_t samples);     // 's'
void cart_diag_addr_lines(void);            // 'a'
void cart_diag_size(void);                  // 'z': probes the ROM size and makes it the diagnostics' bound
uint32_t cart_diag_probe_size(void);        // the same probe, quietly: ROM bytes, in 1 MB steps
void cart_diag_hexdump(uint32_t addr);      // 'x'
void cart_diag_dump(uint32_t mb);           // 'd': raw binary over the console
