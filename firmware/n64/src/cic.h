#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CIC_HALF_US_MAX 30000u  // the SM divider tops out at 65535: 32767 us at 200 MHz

// The cart clock and DCLK; both must settle before the cart leaves reset or the CIC misses bit 0.
void cic_init(void);

// Console 'k': resets the cart, reads the handshake, reports region, seed and variant.
void cic_cmd(void);

// Console 'y': the first DCLK half period 'k' tries.
uint32_t cic_half_us(void);
bool cic_set_half_us(uint32_t us);      // false outside 1..CIC_HALF_US_MAX
