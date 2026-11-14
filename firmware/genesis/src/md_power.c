#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "board.h"
#include "fault.h"
#include "pads.h"
#include "md_power.h"

#define RAIL_MS   50u           // C3 + C4-C8 charging through Q2; TP3 is not on a GPIO
#define CLOCK_MS  150u          // with RAIL_MS, OSCR's 200 ms from power to the first read
#define VCLK_HZ   7670454.0f    // NTSC 68000 clock, 53.693175 MHz / 7

static bool g_present;

void md_power_on(void) {
    MARK(3);
    for (uint p = 0; p <= PIN_CART_LAST; p++) gpio_init(p);
    for (uint p = PIN_VD0; p < PIN_VD0 + 16; p++) gpio_pull_up(p);     // OSCR dataIn_MD: open bus reads FFFF
    pad_drive(PIN_LWR, 1);
    pad_drive(PIN_UWR, 1);
    pad_drive(PIN_CE, 1);
    pad_drive(PIN_OE, 1);
    pad_drive(PIN_AS, 1);
    pad_drive(PIN_ASEL, 1);
    pad_drive(PIN_TIME, 1);
    pad_drive(PIN_CART_RESET, 0);
    pad_drive(PIN_ADDR_SER, 0);
    pad_drive(PIN_ADDR_SRCLK, 0);
    pad_drive(PIN_ADDR_RCLK, 0);
    gpio_init(PIN_CART_DET);
    gpio_pull_up(PIN_CART_DET);
    sleep_us(10);
    g_present = !gpio_get(PIN_CART_DET);

    pad_drive(PIN_CART_PWR_EN, 1);
    pad_drive(PIN_LED, 1);
    sleep_ms(RAIL_MS);
    MARK(4);

    clock_gpio_init(PIN_VCLK, CLOCKS_CLK_GPOUT0_CTRL_AUXSRC_VALUE_CLK_SYS, (float)clock_get_hz(clk_sys) / VCLK_HZ);
    sleep_ms(CLOCK_MS);
    gpio_put(PIN_CART_RESET, 1);
}

bool md_cart_present(void) { return g_present; }
