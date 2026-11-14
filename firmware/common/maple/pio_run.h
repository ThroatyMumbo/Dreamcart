#pragma once
#include "hardware/pio.h"
#include "hardware/clocks.h"

// The SM clock divider for a program written at `hz`, from whatever clk_sys this build runs.
static inline float pio_clkdiv_hz(uint32_t hz) {
    return (float)clock_get_hz(clk_sys) / (float)hz;
}
