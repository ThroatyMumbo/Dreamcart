#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "board_common.h"
#include "sd_int.h"

#define TOKEN_DATA   0xFE
#define TOKEN_MULTI  0xFC
#define TOKEN_STOP   0xFD
#define TOKEN_US     100000u
#define BUSY_US      250000u
#define WRITE_US     500000u
#define INIT_US      1000000u

static uint8_t *const scratch = (uint8_t *)sd_scratch;

// Every bring-up: SDIO may have had the pins since the last one.
static void pins_setup(void) {
    spi_init(SD_SPI, 400000);
    spi_set_format(SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_SD_CLK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SD_CMD, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SD_D0, GPIO_FUNC_SPI);
    gpio_pull_up(PIN_SD_D0);
    for (uint p = PIN_SD_D0 + 1; p < PIN_SD_D3; p++) {     // D1, D2: unused in SPI mode
        gpio_init(p);
        gpio_pull_up(p);
    }
    gpio_init(PIN_SD_D3);
    gpio_put(PIN_SD_D3, 1);
    gpio_set_dir(PIN_SD_D3, GPIO_OUT);
}

static uint8_t xfer(uint8_t b) {
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &b, &r, 1);
    return r;
}

static void deselect(void) {
    gpio_put(PIN_SD_D3, 1);
    xfer(0xFF);
}

static bool wait_ready(uint32_t us) {
    uint32_t t0 = time_us_32();
    while (xfer(0xFF) != 0xFF) {
        if (time_us_32() - t0 > us) return false;
        sd_wait_tick();
    }
    return true;
}

// Leaves CS low: the caller reads whatever follows R1, then deselects.
static uint8_t command(uint8_t idx, uint32_t arg) {
    sd_stats.stage = idx;
    gpio_put(PIN_SD_D3, 0);
    if (idx != 0 && idx != 12) wait_ready(BUSY_US);         // CMD12 cuts into a data stream; CMD0 meets any state
    else xfer(0xFF);
    uint8_t f[6] = {(uint8_t)(0x40 | idx), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8),
                    (uint8_t)arg, 0};
    f[5] = (uint8_t)(sd_crc7(f, 5) << 1 | 1);
    spi_write_blocking(SD_SPI, f, 6);
    if (idx == 12) xfer(0xFF);                              // stuff byte
    for (int i = 0; i < 9; i++) {
        uint8_t r = xfer(0xFF);
        if (!(r & 0x80)) {
            sd_stats.resp = r;
            if (r > 1 && idx != 8) sd_fail(idx, SD_FAIL_RESPONSE);  // CMD8 is illegal on a v1 card, not a fault
            return r;
        }
    }
    sd_fail(idx, SD_FAIL_NO_RESPONSE);
    return 0xFF;
}

static uint8_t acmd(uint8_t idx, uint32_t arg) {
    uint8_t r = command(55, 0);
    deselect();
    return r > 1 ? r : command(idx, arg);
}

static bool read_data(uint8_t *dst, uint32_t n) {
    uint32_t t0 = time_us_32();
    uint8_t tok;
    while ((tok = xfer(0xFF)) == 0xFF)
        if (time_us_32() - t0 > TOKEN_US) {
            sd_fail(sd_stats.stage, SD_FAIL_NO_DATA);
            return false;
        }
    if (tok != TOKEN_DATA) {
        sd_stats.resp = tok;
        sd_fail(sd_stats.stage, SD_FAIL_NO_DATA);
        return false;
    }
    uint8_t c[2];
    spi_read_blocking(SD_SPI, 0xFF, dst, n);
    spi_read_blocking(SD_SPI, 0xFF, c, 2);
    if (sd_crc16(dst, (int)n) != (uint16_t)(c[0] << 8 | c[1])) {
        sd_fail(sd_stats.stage, SD_FAIL_DATA_CRC);
        return false;
    }
    return true;
}

static bool switch_high_speed(sd_card_t *c, bool want) {
    bool ok = command(6, 0x00FFFFF1u) == 0 && read_data(scratch, 64);
    deselect();
    if (!ok) return false;
    c->hs_support = sd_switch_support(scratch);
    if (!want || !(c->hs_support & 0x02)) return false;
    ok = command(6, 0x80FFFFF1u) == 0 && read_data(scratch, 64);
    deselect();
    return ok && sd_switch_selected(scratch);
}

static bool init_once(sd_card_t *c, uint32_t hz) {
    uint8_t r, b[4];
    spi_set_baudrate(SD_SPI, 400000);
    gpio_put(PIN_SD_D3, 1);
    for (int i = 0; i < 10; i++) xfer(0xFF);                // >= 74 clocks with CS high

    r = command(0, 0);
    deselect();
    if (r != 0x01) return false;

    r = command(8, 0x1AA);
    spi_read_blocking(SD_SPI, 0xFF, b, 4);
    deselect();
    bool v2 = r == 0x01 && (b[2] & 0x0F) == 0x01 && b[3] == 0xAA;

    r = command(59, 1);
    deselect();
    if (r > 1) return false;

    uint32_t t0 = time_us_32();
    while ((r = acmd(41, v2 ? 0x40000000u : 0)) != 0) {
        deselect();
        if (r > 1 || time_us_32() - t0 > INIT_US) return false;
        sleep_ms(5);
    }
    deselect();

    r = command(58, 0);
    spi_read_blocking(SD_SPI, 0xFF, b, 4);
    deselect();
    if (r != 0) return false;
    c->ocr = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    c->block_addr = v2 && (c->ocr & 0x40000000u);

    if (!c->block_addr) {
        r = command(16, SD_SECTOR);
        deselect();
        if (r != 0) return false;
    }
    bool ok = command(9, 0) == 0 && read_data(c->csd, 16);
    deselect();
    ok = ok && command(10, 0) == 0 && read_data(c->cid, 16);
    deselect();
    if (!ok) return false;
    c->sectors = sd_csd_sectors(c->csd);
    c->hz = spi_set_baudrate(SD_SPI, hz > SD_DEFAULT_HZ ? SD_DEFAULT_HZ : hz);
    c->high_speed = switch_high_speed(c, hz > SD_DEFAULT_HZ);
    if (c->high_speed) c->hz = spi_set_baudrate(SD_SPI, hz > SD_MAX_HZ ? SD_MAX_HZ : hz);
    sd_stats.stage = 100;
    return true;
}

// A card that answered once now and then misses a step of the bring-up; the whole sequence goes again.
bool sd_spi_init(sd_card_t *c, uint32_t hz) {
    pins_setup();
    for (int i = 0; i < 3; i++)
        if (init_once(c, hz)) return true;
    return false;
}

static bool read_block(uint8_t *dst) {
    if (dst) return read_data(dst, SD_SECTOR);
    if (!read_data(scratch, SD_SECTOR)) return false;
    sd_stats.sum += sd_word_sum(sd_scratch, SD_SECTOR / 4);
    return true;
}

static bool write_block(uint8_t token, const uint8_t *src) {
    if (!wait_ready(WRITE_US)) {
        sd_fail(sd_stats.stage, SD_FAIL_NO_DATA);
        return false;
    }
    uint16_t crc = sd_crc16(src, SD_SECTOR);
    xfer(token);
    spi_write_blocking(SD_SPI, src, SD_SECTOR);
    xfer((uint8_t)(crc >> 8));
    xfer((uint8_t)crc);
    uint8_t r = xfer(0xFF);
    sd_stats.resp = r;
    if ((r & 0x1F) != 0x05) {                               // xxx0 010 1: accepted
        sd_fail(sd_stats.stage, SD_FAIL_WRITE);
        return false;
    }
    return true;
}

bool sd_spi_write(const sd_card_t *c, uint32_t sector, uint32_t blocks, const uint8_t *src) {
    uint32_t addr = c->block_addr ? sector : sector * SD_SECTOR;
    bool ok;
    if (blocks == 1) {
        ok = command(24, addr) == 0 && write_block(TOKEN_DATA, src);
    } else if ((ok = command(25, addr) == 0)) {
        for (uint32_t k = 0; ok && k < blocks; k++) ok = write_block(TOKEN_MULTI, src + SD_SECTOR * k);
        ok = wait_ready(WRITE_US) && ok;
        xfer(TOKEN_STOP);
        xfer(0xFF);
    }
    ok = wait_ready(WRITE_US) && ok;
    deselect();
    return ok;
}

bool sd_spi_read(const sd_card_t *c, uint32_t sector, uint32_t blocks, uint8_t *dst) {
    uint32_t addr = c->block_addr ? sector : sector * SD_SECTOR;
    bool ok;
    if (blocks == 1) {
        ok = command(17, addr) == 0 && read_block(dst);
    } else if ((ok = command(18, addr) == 0)) {
        for (uint32_t k = 0; ok && k < blocks; k++) ok = read_block(dst ? dst + SD_SECTOR * k : NULL);
        ok = command(12, 0) == 0 && ok;
        ok = wait_ready(BUSY_US) && ok;
    }
    deselect();
    return ok;
}
