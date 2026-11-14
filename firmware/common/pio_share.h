// One PIO block's instruction memory, time-shared by users that never run at once. Core0 only.
#pragma once
#include "hardware/pio.h"

typedef struct {
    void (*load)(void);             // add the programs and start the SMs; claimed SMs persist across swaps
    void (*unload)(void);           // stop the SMs and remove the programs
} pio_tenant_t;

void pio_share_enter(PIO pio, const pio_tenant_t *t);   // NULL evicts whoever holds the block
void pio_share_leave(PIO pio, const pio_tenant_t *t);   // unloads t if it holds the block
