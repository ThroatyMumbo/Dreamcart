#pragma once
#include <stdbool.h>

// Core0, before md_bus_init: the docs/genesis_board.md power-up contract, VCLK running, reset released.
void md_power_on(void);

// CART_DET as sampled on its pull-up before the rail came up; unconfirmed on a real cart, so never gates anything.
bool md_cart_present(void);
