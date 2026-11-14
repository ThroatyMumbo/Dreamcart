#pragma once
#include <stdbool.h>
#include <stdint.h>

// BLOCK_SYNC ops past cart_pass's 1 and 2
#define CART_SYNC_POWER_OFF 3u
#define CART_SYNC_POWER_ON  4u

// Core0, before n64_bus_init: raises CART_PWR_EN and the LED and waits for CART_3V3.
void cart_power_on(void);

// Core0, after n64_bus_init and cic_init: lets the CIC clock settle, then lifts cart reset.
void cart_reset_release(void);

// Core1, as maple_cart_t.ready: false while the cart is powered off for a swap.
bool cart_ready(void);

// Console 'h': power the cart off, wait for Enter, power the new one up and reload everything served from it.
void cart_swap(void);

// Core1: takes a power op for core0; false while the last one is still being carried out.
bool cart_power_sync(uint32_t op);

// Core0 main loop: carries out the power op cart_power_sync took.
void cart_power_service(void);
