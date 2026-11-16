// The boot status screen on the framebuffer, shown until the PVR takes the display
#ifndef LOADING_H
#define LOADING_H

#include <stddef.h>
#include <stdint.h>

#include "cartboot.h"

enum { LOADING_COMMIT = CB_STAGE_N, LOADING_START, LOADING_N };

void loading_open(const char *title, uint32_t rom_bytes);
// A name with no spaces gets one before a number after 3+ letters (within cap); then an
// all-caps name goes to title case
void loading_title_case(char *s, size_t cap);
// The cart header's 20-byte title as shown: printable, trailing spaces gone, then loading_title_case
void loading_cart_title(const uint8_t *hdr, char *out, size_t cap);
void loading_stage(int stage);                               // any thread: plain stores, drawn by loading_draw
// A bar for stage once *total is known; shift 10 shows bytes as KB
void loading_progress(int stage, const volatile uint32_t *done, const volatile uint32_t *total, unsigned shift);
void loading_draw(void);                                     // the main thread only

#endif
