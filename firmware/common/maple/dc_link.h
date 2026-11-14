#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mapledev.h"
#include "maple_cart.h"

// Claims all of PIO_MAPLE for Maple on the board_common.h pins and starts core1 answering as an HKT-7700 controller
// with the cart reader as sub-unit 2; cart and id must outlive the link, and cart's callbacks run on core1.
bool dc_link_start(const maple_cart_t *cart, const maple_devid_t *id);
bool dc_link_running(void);

// MAPLE_BTN_* bits held down, active high here; 0 releases everything.
void dc_link_set_buttons(uint16_t pressed);

// Analog triggers, 0 released to 255 fully pulled.
void dc_link_set_triggers(uint8_t left, uint8_t right);

const mapledev_stats_t *dc_link_stats(void);
