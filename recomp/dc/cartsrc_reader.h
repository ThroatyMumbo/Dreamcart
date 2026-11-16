// The cart reader as cartboot's source: its ROM pass tables, its flash cache of the scan, its SD cache of translations
#ifndef CARTSRC_READER_H
#define CARTSRC_READER_H

#include <stdint.h>
#include "cartboot.h"

// boot_out is the boot translation, whose SD entry waits for reader_upload_start
void reader_src(CartSrc *src, uint32_t rom_size, const XlOut *boot_out);
// Waits out the reader's flash write of the scan cache, calling idle between polls; 1 once it is stored
int reader_commit_wait(void (*idle)(void));
void reader_upload_start(uint32_t text_crc);  // the boot translation to the SD cache, on its own thread
int reader_upload_spare(void **mem, uint32_t *len);  // once, after the upload: its buffer for the ROM cache
void reader_take_reads(uint32_t *calls, uint32_t *bytes, uint64_t *us);  // ROM reads since the last call
extern volatile uint32_t reader_xc_done, reader_xc_total;  // bytes of an SD entry moved, for the loading screen

#endif
