// microSD on CLK GP26, CMD GP27, D0-D3 GP28-31, card detect GP34: 4-bit SDIO on PIO_SD, SPI1 as the fallback.
// Core 0, blocking.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SD_SECTOR     512u          // bytes a block, and the block length CMD16 sets on byte-addressed cards
#define SD_DEFAULT_HZ 25000000u     // default speed: the most a card takes before CMD6
#define SD_MAX_HZ     50000000u     // high speed, after CMD6 switches function group 1

#define SD_FAIL_NO_RESPONSE 0x100u  // sd_stats.fail: command index | one of these
#define SD_FAIL_RESPONSE    0x200u  // error bits, bad CRC or wrong index in the response
#define SD_FAIL_NO_DATA     0x400u  // no data block, or an error token
#define SD_FAIL_DATA_CRC    0x800u
#define SD_FAIL_OVERRUN     0x1000u // a discarded read outran its CRC check
#define SD_FAIL_WRITE       0x2000u // the card did not accept a written block

typedef struct {
    uint32_t reads, writes, crc_errors, timeouts, cmd_errors, overruns, write_errors;
    uint32_t stage;                 // last command a bring-up sent, 100 once up
    uint32_t resp;                  // last response or data token
    uint32_t fail;                  // last failure, SD_FAIL_*
    uint32_t sdio_fail;             // why SDIO bring-up failed when SPI came up instead, 0 otherwise
    uint32_t sum;                   // 32-bit word sum of every block read with dst NULL
} sd_stats_t;
extern sd_stats_t sd_stats;

// Run from the drivers' busy waits (a write's card busy, mostly); it must leave the SD, its PIO and DMA alone
extern void (*sd_wait_hook)(void);

typedef enum { SD_MODE_SDIO, SD_MODE_SPI } sd_mode_t;

typedef struct {
    sd_mode_t mode;
    bool block_addr;                // SDHC/SDXC: addressed in sectors, not bytes
    bool high_speed;                // CMD6 switched the card to high speed
    uint16_t hs_support;            // CMD6 group 1 support bits; bit 1 = high speed
    uint32_t ocr, sectors, hz;
    uint8_t cid[16], csd[16];
} sd_card_t;

bool sd_present(void);
// SDIO first unless force_spi: a card that has seen SPI stays in SPI until it loses power.
bool sd_init(uint32_t hz, bool force_spi);
const sd_card_t *sd_card(void);     // NULL until sd_init has succeeded
// dst word-aligned; NULL: CRC-checked, added to sd_stats.sum and discarded.
bool sd_read(uint32_t sector, uint32_t blocks, uint8_t *dst);
bool sd_write(uint32_t sector, uint32_t blocks, const uint8_t *src);   // src word-aligned
