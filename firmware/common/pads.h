#pragma once
#include "hardware/gpio.h"

// An SIO output, its level latched before the pin turns around
static inline void pad_drive(uint pin, bool level) {
    gpio_init(pin);
    gpio_put(pin, level);
    gpio_set_dir(pin, GPIO_OUT);
}

// 8 mA and fast slew, for cart bus lines
static inline void pad_fast(uint pin) {
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_8MA);
    gpio_set_slew_rate(pin, GPIO_SLEW_RATE_FAST);
}
