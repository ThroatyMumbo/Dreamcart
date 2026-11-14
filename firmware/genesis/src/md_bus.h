#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MD_BUS_HZ_DEFAULT 20000000u     // PIO cycle; SRCLK runs at half of it
#define MD_BUS_HZ_MIN     1000000u
#define MD_BUS_HZ_MAX     50000000u

#define MD_STROBE_CE      (1u << 0)
#define MD_STROBE_LWR     (1u << 1)
#define MD_STROBE_UWR     (1u << 2)

// Core0, after md_power_on. Claims two SMs of PIO_CART_BUS and two DMA channels.
void md_bus_init(void);

// Addresses are word addresses (VA1-23: byte address >> 1). Reads land in .bin byte order, high byte first.
void md_bus_read(uint32_t word, uint8_t *dst, uint32_t nwords);
void md_bus_write(uint32_t word, const uint16_t *src, uint32_t nwords, uint32_t strobes);

// A 68000 byte write to the TIME range: /TIME held low across one /LWR cycle (OSCR writeSSF2Map).
void md_bus_reg_write(uint32_t word, uint16_t val);

// The PIO cycle; access and strobe times stay fixed in ns. False outside MD_BUS_HZ_MIN..MAX.
bool md_bus_set_hz(uint32_t hz);
uint32_t md_bus_hz(void);
