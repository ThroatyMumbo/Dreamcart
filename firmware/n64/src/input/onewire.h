// onewire.h -- generic single-wire bidirectional serial transport (controller bus).
//
// Half-duplex 1-wire link: the host sends a request frame on one open-drain data line, then the
// device replies with a fixed number of bytes (wire encoding in onewire.pio). Protocol-agnostic --
// it shifts bytes both ways and knows nothing about their meaning; input/n64.c is the first user.
//
// Runs on pio2 (free; pio0 carries video + audio), no DMA/IRQ, blocking on core0. Up to
// ONEWIRE_MAX_BUSES independent links, one per pio2 SM, selected by `id`. The shared PIO program
// loads once; each `id` gets its own SM + data pin. Data pins may live in GPIO16..47, so the first
// onewire_init sets pio2's GPIO base to 16 -- all data pins must be >= 16.
#ifndef ONEWIRE_H
#define ONEWIRE_H

#include <stdint.h>
#include <stdbool.h>

// One bus per pio2 state machine.
#define ONEWIRE_MAX_BUSES 4

// Claim a pio2 state machine for bus `id` and bring its data line up on `data_pin`. The first call
// also loads the shared PIO program and sets pio2's GPIO base to 16 (so pins in GPIO16..47 work).
// Returns true on success.
bool onewire_init(uint8_t id, uint8_t data_pin);

// Run one transaction on bus `id`: send `request_len` bytes, then read `response_len` bytes into
// `response`. Blocking. Bytes that miss the per-byte timeout are returned as 0. Returns the count
// that actually arrived (< response_len = device silent/absent) -- callers use this for presence.
uint8_t onewire_transfer(uint8_t id, const uint8_t *request, uint8_t request_len, uint8_t *response,
                         uint8_t response_len);

#endif // ONEWIRE_H
