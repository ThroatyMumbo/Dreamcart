#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "board_common.h"
#include "sd_int.h"

sd_stats_t sd_stats;
void (*sd_wait_hook)(void);
uint32_t sd_scratch[SD_SCRATCH_WORDS];
static sd_card_t card;
static bool card_up, det_up, tables_up;
static uint16_t crc16_tab[256];

uint8_t sd_crc7(const uint8_t *p, int n) {
    uint8_t c = 0;
    while (n--) {
        uint8_t x = *p++;
        for (int b = 7; b >= 0; b--) {
            bool in = ((x >> b) ^ (c >> 6)) & 1;
            c = (uint8_t)((c << 1) & 0x7F);
            if (in) c ^= 0x09;
        }
    }
    return c;
}

uint16_t sd_crc16(const uint8_t *p, int n) {
    uint16_t c = 0;
    while (n--) c = (uint16_t)(c << 8 ^ crc16_tab[(c >> 8 ^ *p++) & 0xFF]);
    return c;
}

static void tables(void) {
    for (int i = 0; i < 256; i++) {
        uint16_t d = (uint16_t)(i << 8);
        for (int k = 0; k < 8; k++) d = d & 0x8000 ? (uint16_t)(d << 1 ^ 0x1021) : (uint16_t)(d << 1);
        crc16_tab[i] = d;
    }
    tables_up = true;
}

uint32_t sd_csd_sectors(const uint8_t *csd) {
    if (csd[0] >> 6 == 1) return ((((uint32_t)csd[7] & 0x3F) << 16 | (uint32_t)csd[8] << 8 | csd[9]) + 1) * 1024;
    if (csd[0] >> 6 != 0) return 0;
    uint32_t c_size = ((uint32_t)csd[6] & 0x03) << 10 | (uint32_t)csd[7] << 2 | csd[8] >> 6;
    uint32_t mult = ((uint32_t)csd[9] & 0x03) << 1 | csd[10] >> 7, bl_len = csd[5] & 0x0F;
    return (c_size + 1) << (mult + 2 + bl_len - 9);
}

uint32_t sd_word_sum(const uint32_t *p, uint32_t words) {
    uint32_t s = 0;
    while (words--) s += *p++;
    return s;
}

void sd_fail(uint32_t idx, uint32_t why) {
    sd_stats.fail = idx | why;
    if (why == SD_FAIL_NO_RESPONSE || why == SD_FAIL_RESPONSE) sd_stats.cmd_errors++;
    else if (why == SD_FAIL_NO_DATA) sd_stats.timeouts++;
    else if (why == SD_FAIL_DATA_CRC) sd_stats.crc_errors++;
    else if (why == SD_FAIL_OVERRUN) sd_stats.overruns++;
    else if (why == SD_FAIL_WRITE) sd_stats.write_errors++;
}

bool sd_present(void) {
    if (!det_up) {
        gpio_init(PIN_SD_DET);
        gpio_disable_pulls(PIN_SD_DET);                     // the module pulls it up
        busy_wait_us(100);                                  // the first read right after gpio_init came back low
        det_up = true;
    }
    return gpio_get(PIN_SD_DET);
}

bool sd_init(uint32_t hz, bool force_spi) {
    sd_present();
    if (!tables_up) tables();
    card_up = false;
    memset(&card, 0, sizeof card);
    sd_stats.fail = sd_stats.sdio_fail = 0;
    if (!force_spi) {
        if (sd_sdio_init(&card, hz)) {
            card.mode = SD_MODE_SDIO;
            return card_up = true;
        }
        sd_stats.sdio_fail = sd_stats.fail;
        memset(&card, 0, sizeof card);
    }
    sd_sdio_release();
    card.mode = SD_MODE_SPI;
    return card_up = sd_spi_init(&card, hz);
}

const sd_card_t *sd_card(void) { return card_up ? &card : NULL; }

bool sd_read(uint32_t sector, uint32_t blocks, uint8_t *dst) {
    if (!card_up || blocks == 0 || ((uintptr_t)dst & 3)) return false;
    sd_stats.fail = 0;
    bool ok = card.mode == SD_MODE_SDIO ? sd_sdio_read(&card, sector, blocks, dst)
                                        : sd_spi_read(&card, sector, blocks, dst);
    sd_stats.reads++;
    return ok;
}

bool sd_write(uint32_t sector, uint32_t blocks, const uint8_t *src) {
    if (!card_up || blocks == 0 || ((uintptr_t)src & 3) || sector + blocks > card.sectors) return false;
    sd_stats.fail = 0;
    bool ok = card.mode == SD_MODE_SDIO ? sd_sdio_write(&card, sector, blocks, src)
                                        : sd_spi_write(&card, sector, blocks, src);
    sd_stats.writes++;
    return ok;
}
