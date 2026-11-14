#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "board.h"
#include "fault.h"
#include "pads.h"
#include "snes_power.h"

#define RAIL_MS   50u           // C3 + C4-C7 charging through Q2; TP3 is not on a GPIO
#define CLOCK_MS  100u
#define MCLK_HZ   21477270.0f
#define CICCLK_HZ 3072000.0f

static void gpout(uint pin, float hz) {
    clock_gpio_init(pin, CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS, (float)clock_get_hz(clk_sys) / hz);
}

void snes_power_on(void) {
    MARK(3);
    for (uint p = 0; p <= PIN_CART_LAST; p++) gpio_init(p);
    pad_drive(PIN_WR, 1);
    pad_drive(PIN_RD, 1);
    pad_drive(PIN_CART_RESET, 0);
    pad_drive(PIN_CART_PWR_EN, 1);
    pad_drive(PIN_LED, 1);
    sleep_ms(RAIL_MS);

    pad_drive(PIN_ROMSEL, 1);
    pad_drive(PIN_CIC_RST, 1);      // SA-1 is unsupported, so the cart CIC stays in reset
    pad_drive(PIN_REFRESH, 0);      // OSCR holds it low for SA-1; harmless on the rest
    pad_drive(PIN_WRAM, 1);
    pad_drive(PIN_PARD, 1);
    pad_drive(PIN_PAWR, 1);
    pad_drive(PIN_CPUCLK, 0);       // OSCR runs it only for SA-1 BW-RAM writes
    pad_drive(PIN_ADDR_SER, 0);
    pad_drive(PIN_ADDR_SRCLK, 0);
    pad_drive(PIN_ADDR_RCLK, 0);
    MARK(4);

    gpout(PIN_CICCLK, CICCLK_HZ);   // OSCR: CIC clock before the master clock
    gpout(PIN_MCLK, MCLK_HZ);
    sleep_ms(CLOCK_MS);
    gpio_put(PIN_CART_RESET, 1);
}
