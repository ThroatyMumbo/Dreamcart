#include "pico/stdlib.h"
#include "pico/stdio_uart.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "board_common.h"
#include "fault.h"
#include "boot.h"

void boot_init(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    sleep_ms(2);
    set_sys_clock_khz(SYS_CLOCK_KHZ, true);
    stdio_uart_init();
    fault_report();
}

void boot_done(void) { watchdog_disable(); }
