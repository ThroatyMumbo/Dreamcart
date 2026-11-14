#pragma once

// Core0, before snes_bus_init: the docs/snes_board.md power-up contract, cart clocks in OSCR's order, reset released.
void snes_power_on(void);
