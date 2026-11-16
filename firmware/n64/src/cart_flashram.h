#pragma once
#include <stdbool.h>
#include <stdint.h>

#define FRAM_BYTES 131072u
#define FRAM_PAGE  128u

// Console 'f': prints the FlashRAM id; returns its row in the id table, or -1 if none answered.
int cart_flashram_identify(void);

// Console 'r': the whole 128 KB save as raw binary, read from the chip.
void cart_flashram_dump(void);

// Core0, at attach: drops the last cart's chip and requests, then with probe finds this one's; false if none.
bool cart_flashram_attach(bool probe);
bool cart_flashram_present(void);
const char *cart_flashram_name(void);
void cart_flashram_id(uint32_t id[2]);  // the E1 reply, 0 with no chip

// Core0 main loop: one staged read or one step of a page's erase, program and read-back.
void cart_flashram_service(void);
void cart_flashram_service_idle(void);  // runs the page in flight to its end, bounded

// Core1, for the Maple cart unit. A read is served from what core0 staged, else posted and refused (NOT_READY);
// a write takes one whole page while none is in flight.
bool fram_read(uint32_t off, uint32_t len, uint8_t *dst);
bool fram_write(uint32_t off, uint32_t len, const uint8_t *src);
void fram_status(uint32_t *pending, uint32_t *given_up);
