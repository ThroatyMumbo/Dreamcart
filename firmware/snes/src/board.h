// SNES cart reader on a PGA2350; nets and cart pins in docs/snes_board.md.
#pragma once
#include "hardware/pio.h"

#define PIN_D0          0       // D0-7 on GP0-7
#define PIN_RD          8       // /RD, /WR: one set group for snes_bus.pio
#define PIN_WR          9
#define PIN_ROMSEL      10
#define PIN_CART_RESET  11
#define PIN_CIC_D0      12
#define PIN_CIC_D1      13
#define PIN_CIC_RST     14
#define PIN_IRQ         15
#define PIN_REFRESH     16
#define PIN_WRAM        17
#define PIN_ADDR_SER    18      // SER, SRCLK, RCLK: consecutive for snes_bus.pio
#define PIN_ADDR_SRCLK  19
#define PIN_ADDR_RCLK   20
#define PIN_MCLK        21      // GPOUT0
#define PIN_PARD        22
#define PIN_CPUCLK      23      // GPOUT1
#define PIN_CICCLK      24      // GPOUT2
#define PIN_PAWR        25
#define PIN_CART_LAST   25

#define PIN_CART_PWR_EN 35
#define PIN_LED         36

#define PIO_CART_BUS    pio0
// pio1 is free; pio2 is Maple's, claimed whole by dc_link
