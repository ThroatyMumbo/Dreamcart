#ifndef PI_LEDGER_H
#define PI_LEDGER_H

#include <stdint.h>

// Where recent PI DMAs put ROM bytes in RDRAM, for the AICA path to find a sample's ROM copy (alist_aica.c)
void pi_ledger_add(uint32_t rom, uint32_t dram, uint32_t len);  // rcp.c, on every PI DMA
uint32_t pi_ledger_rom(uint32_t dram);  // the ROM offset the newest DMA over dram copied it from, else ~0u

#endif
