#pragma once
#include <stdint.h>

// Console bring-up tests on the cart bus, each naming a fault by the line and cart edge pad to probe.
void cart_diag_open_lines(void);            // 'o'
void cart_diag_addr_lines(void);            // 'a'
void cart_diag_hexdump(uint32_t addr);      // 'x': byte address
void cart_diag_dump(uint32_t kb);           // 'd': raw ROM in file order over the console; 0 means served
void cart_diag_verify(void);                // 'v': header checksum of each part
