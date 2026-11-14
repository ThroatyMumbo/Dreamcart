#pragma once
#include <stdint.h>

// Console bring-up tests on the cart bus, each naming a fault by the line and cart edge pin to probe.
void cart_diag_open_lines(void);            // 'o'
void cart_diag_addr_lines(void);            // 'a'
void cart_diag_hexdump(uint32_t bus);       // 'x'
void cart_diag_dump(uint32_t kb);           // 'd': raw ROM in file order over the console; 0 means rom_size
void cart_diag_verify(void);                // 'v': checksum pass and real-size report
