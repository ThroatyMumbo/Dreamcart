#pragma once
#include "hardware/structs/watchdog.h"

// boot breadcrumb; scratch survives lockup, so this says how far we got
#define MARK(n) do { watchdog_hw->scratch[6] = 0xB0070000u | (n); } while (0)

// Prints, once, what the HardFault handler saved before the reboot it forced.
void fault_report(void);
