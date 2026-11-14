#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SNES_BUS_HZ_DEFAULT 20000000u   // PIO cycle; SRCLK runs at half of it
#define SNES_BUS_HZ_MIN     1000000u
#define SNES_BUS_HZ_MAX     50000000u

// Core0, after snes_power_on. Claims both SMs' worth of PIO_CART_BUS and two DMA channels.
void snes_bus_init(void);

// Any length. /ROMSEL follows the S-CPU's decode, and the address wraps inside its bank as the S-CPU's does.
void snes_bus_read(uint32_t bus, uint8_t *dst, uint32_t n);
void snes_bus_write(uint32_t bus, const uint8_t *src, uint32_t n);

// The PIO cycle; access and /WR times stay fixed in ns. False outside SNES_BUS_HZ_MIN..MAX.
bool snes_bus_set_hz(uint32_t hz);
uint32_t snes_bus_hz(void);
