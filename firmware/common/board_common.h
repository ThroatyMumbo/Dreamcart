// What every cart reader board shares: PGA2350, Maple on GP32/GP33, UART1 console on GP40/GP41 (set in dccart_common.cmake).
#pragma once

#define PIN_CART_PWR_EN 35      // U1 EN; R5 holds it low
#define PIN_LED         36

#define PIN_MAPLE_SDCKA 32      // SDCKB must be the next pin up
#define PIN_MAPLE_SDCKB 33
#define PIO_MAPLE       pio2    // claimed whole by dc_link

#define PIN_SD_CLK      26      // SPI1 SCK
#define PIN_SD_CMD      27      // SPI1 TX
#define PIN_SD_D0       28      // SPI1 RX
#define PIN_SD_D3       31      // CS in SPI mode
#define PIN_SD_DET      34      // high = card in the slot
#define SD_SPI          spi1
#define PIO_SD          pio1    // SDIO; on N64 time-shared with the joybus EEPROM link

#define SYS_CLOCK_KHZ   200000  // below 200 MHz the Maple decoder stalls and drops block reads
