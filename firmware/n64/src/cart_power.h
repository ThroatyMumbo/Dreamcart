#pragma once
#include <stdbool.h>
#include <stdint.h>

// Core0, before n64_bus_init: raises CART_PWR_EN and the LED and waits for CART_3V3.
void cart_power_on(void);

// Core0, after the bus and CIC clock are up: lifts cart reset, brings up the save chip (first: init, else reload),
// the ROM stream and the ROM size. Power-on and swap both run it.
void cart_bringup(bool first);

// Core0, once PSRAM is up: this cart's pass and flash cache slot, and its key for the SD cache.
void cart_attach(void);

// Core1, as maple_cart_t.ready: false while the cart is powered off for a swap.
bool cart_ready(void);

// Console 'h': power the cart off, wait for Enter, power the new one up and reload everything served from it.
void cart_swap(void);

// Core1: takes CART_SYNC_POWER_OFF or _ON for core0; false while the last one is still being carried out.
bool cart_power_sync(uint32_t op);

// Core0 main loop: carries out the power op cart_power_sync took.
void cart_power_service(void);
