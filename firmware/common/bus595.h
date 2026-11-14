#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "hardware/pio.h"
#include "hardware/dma.h"

// The SNES and MD cart bus: a read SM and a write SM, each shifting the address into a 595 chain.
typedef struct {
    PIO pio;
    uint rd_sm, wr_sm, rd_off, wr_off, tx_dma, rx_dma;
    uint32_t hz;
} bus595_t;

// Core0: fast pads, both programs loaded, the ctl pins driven at idle and handed to the PIO, two DMA channels.
// The caller then inits each SM and calls bus595_set_hz, which starts them.
const bus595_t *bus595_init(PIO pio, const pio_program_t *rd, const pio_program_t *wr,
                            uint32_t ctl, uint32_t data, uint32_t idle);

// The PIO cycle: the read SM's Y and the write SM's ISR keep access_ns and wr_ns, each less 3 fixed cycles.
void bus595_set_hz(uint32_t hz, uint32_t access_ns, uint32_t wr_ns);

// One burst of n tab words through the read SM; its n results land at dst, size bytes each.
void bus595_read(const uint32_t *tab, void *dst, uint32_t n, enum dma_channel_transfer_size size, bool bswap);

// One burst of n tab words through the write SM; returns once it is idle again.
void bus595_write(const uint32_t *tab, uint32_t n);
