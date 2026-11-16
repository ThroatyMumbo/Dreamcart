#ifndef FLASHRAM_H
#define FLASHRAM_H

#include "htypes.h"

// The cart's 128 KB FlashRAM at PI domain 2 (0x08000000), as libultra's osFlash* drives it; absent until loaded
#define FLASH_BYTES 0x20000u
#define FLASH_PAGE  128u
#define FLASH_BASE  0x08000000u

// img stays the caller's: N64 byte order, FLASH_BYTES long. The id is the chip's E1 reply: type, then maker.
void flash_load(uint8_t *img, uint32_t id_type, uint32_t id_maker);
int flash_present(void);
int flash_claims(uint32_t phys);  // 0x08000000-0x0FFFFFFF with a chip loaded
uint32_t flash_rd(uint32_t phys);
void flash_wr(uint32_t phys, uint32_t v);
void flash_dma_read(uint32_t dram, uint32_t off, uint32_t len);   // chip to RDRAM
void flash_dma_write(uint32_t dram, uint32_t off, uint32_t len);  // RDRAM to the chip's page buffer
// Each page an erase or a program changed, after the image has it (the console passes it on to the real cart)
extern void (*flash_on_page)(uint32_t page);

#endif
