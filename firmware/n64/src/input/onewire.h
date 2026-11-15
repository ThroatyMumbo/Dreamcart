// Protocol-agnostic half-duplex joybus link on PIO_JOYBUS (pio1, shared with SDIO), blocking; used by cart_eeprom.c.
#ifndef ONEWIRE_H
#define ONEWIRE_H

#include <stdint.h>
#include <stdbool.h>

// One bus per state machine.
#define ONEWIRE_MAX_BUSES 4

// Claim an SM for bus `id` on `data_pin` (>= 16: the first call sets pio1's GPIO base to 16). False if none is free.
bool onewire_init(uint8_t id, uint8_t data_pin);

// Send `request_len` bytes, then read `response_len`; missed bytes read 0. Returns how many arrived.
uint8_t onewire_transfer(uint8_t id, const uint8_t *request, uint8_t request_len, uint8_t *response,
                         uint8_t response_len);

#endif // ONEWIRE_H
