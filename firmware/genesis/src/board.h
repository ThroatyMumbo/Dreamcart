// Mega Drive cart reader on a PGA2350; nets and cart pins in docs/genesis_board.md.
#pragma once
#include "hardware/pio.h"

#define PIN_VD0         0       // VD0-15 on GP0-15
#define PIN_CE          16      // /CE0, /OE (CAS0), /AS, /ASEL: one set group for md_bus.pio
#define PIN_OE          17
#define PIN_AS          18
#define PIN_ASEL        19
#define PIN_ADDR_SER    20      // SER, SRCLK, RCLK: consecutive for md_bus.pio
#define PIN_ADDR_SRCLK  21
#define PIN_ADDR_RCLK   22
#define PIN_VCLK        23      // GPOUT1
#define PIN_LWR         24
#define PIN_UWR         25
#define PIN_CART_LAST   25

#define PIN_CART_PWR_EN 35
#define PIN_LED         36
#define PIN_TIME        37      // outside pio0's window, so a static SIO select
#define PIN_CART_RESET  38      // VRES and MRES
#define PIN_CART_DET    39

#define PIO_CART_BUS    pio0
// pio1 is free; pio2 is Maple's, claimed whole by dc_link
