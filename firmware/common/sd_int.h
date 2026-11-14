// Shared by the SD front end (sd.c) and its two backends; not for callers.
#pragma once
#include "sd.h"

#define SD_SCRATCH_WORDS 1024u
extern uint32_t sd_scratch[SD_SCRATCH_WORDS];   // discarded reads and CMD6 status

uint8_t sd_crc7(const uint8_t *p, int n);
uint16_t sd_crc16(const uint8_t *p, int n);
uint32_t sd_csd_sectors(const uint8_t *csd);
uint32_t sd_word_sum(const uint32_t *p, uint32_t words);
void sd_fail(uint32_t idx, uint32_t why);       // counts it and records idx | why in sd_stats.fail

static inline void sd_wait_tick(void) {
    if (sd_wait_hook) sd_wait_hook();
}

// CMD6 status: byte 13 bit 1 = group 1 supports high speed; byte 16 low nibble = group 1's function after the call.
static inline uint16_t sd_switch_support(const uint8_t *st) { return (uint16_t)(st[12] << 8 | st[13]); }
static inline bool sd_switch_selected(const uint8_t *st) { return (st[16] & 0x0F) == 1; }

bool sd_spi_init(sd_card_t *c, uint32_t hz);
bool sd_spi_read(const sd_card_t *c, uint32_t sector, uint32_t blocks, uint8_t *dst);
bool sd_spi_write(const sd_card_t *c, uint32_t sector, uint32_t blocks, const uint8_t *src);

bool sd_sdio_init(sd_card_t *c, uint32_t hz);
bool sd_sdio_read(const sd_card_t *c, uint32_t sector, uint32_t blocks, uint8_t *dst);
bool sd_sdio_write(const sd_card_t *c, uint32_t sector, uint32_t blocks, const uint8_t *src);
void sd_sdio_release(void);                     // stops SDIO and frees PIO_SD before SPI takes the pins
