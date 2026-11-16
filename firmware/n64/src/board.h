// N64 cart reader on a PGA2350; nets and cart pins in docs/board.md.
#pragma once
#include "hardware/pio.h"
#include "board_common.h"

#define AD_BASE      0      // AD0-15 on GP0-15
#define PIN_ALE_L    16     // ALE_L, ALE_H, /RD, /WR: one consecutive group for n64_bus.pio
#define PIN_ALE_H    17
#define PIN_RD       18
#define PIN_WR       19
#define PIN_RESET    20
#define PIN_CIC_CLK  21     // cart 19, ~1.95 MHz to the CIC and the EEPROM
#define PIN_CIC_DCLK 22     // cart 43, PIF -> CIC
#define PIN_CIC_DIO  23     // cart 18, CIC -> PIF
#define PIN_S_DAT    24     // cart 21, joybus to the save EEPROM
#define PIN_CART_LAST 25    // CART_NMI, cart 45, never driven; GP0-25 are every cart line

#define PIO_CART_BUS pio0
#define PIO_JOYBUS   pio1   // time-shared with SDIO (pio_share.h); the CIC reader takes it for a capture
// pio2 is Maple's, claimed whole by dc_link

#define CART_ROM     0x10000000u
#define CART_SAVE    0x08000000u
#define FRAM_CMD     0x08010000u

// Who owns which PSRAM bytes
#define PSRAM_PASS_STARTS 0x00000000u   // the whole-ROM pass: its jr ra starts, 512 KB
#define PSRAM_XC_INDEX    0x00080000u   // SD translation cache: the card's index, mirrored, 128 KB
#define PSRAM_XC_STAGE    0x000A0000u   // SD translation cache: the entry in flight, to the end of PSRAM
