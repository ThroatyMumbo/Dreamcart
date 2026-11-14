#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "board.h"
#include "console.h"
#include "cart_sram.h"
#include "input/onewire.h"
#include "cart_eeprom.h"

#define EEP_BUS        0
#define EEP_BUSY       0x80u
#define EEP_MAX        2048u
#define EEP_WRITE_MS   50u      // a block's write cycle is ~15 ms on the chip
#define EEP_POLL_US    500u
#define EEP_AGREE_TRIES 6u      // whole-chip reads at boot until two in a row match

_Static_assert(CART_SAVE_MAX == EEP_MAX && CART_SAVE_BLOCK == 8u, "n64/CMakeLists.txt sizes cart_sram for the EEPROM");

// 512 (4 Kbit), 2048 (16 Kbit), or 0 when nothing answers the status command
static uint32_t eep_status(bool *busy) {
    const uint8_t req = 0x00;
    uint8_t resp[3];
    if (onewire_transfer(EEP_BUS, &req, 1, resp, 3) != 3) return 0;
    if (busy) *busy = resp[2] & EEP_BUSY;
    return resp[1] == 0x80 ? 512u : resp[1] == 0xC0 ? 2048u : 0u;
}

static bool eep_read(uint32_t off, uint8_t *dst, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 8) {
        const uint8_t req[2] = { 0x04, (uint8_t)((off + i) / 8) };
        if (onewire_transfer(EEP_BUS, req, 2, dst + i, 8) != 8) return false;
    }
    return true;
}

// Starts the chip's write cycle on one 8-byte block (n is always 8); cart_sram polls eep_busy until it ends.
static bool eep_write(uint32_t off, const uint8_t *src, uint32_t n) {
    (void)n;
    uint8_t req[10] = { 0x05, (uint8_t)(off / 8) };
    uint8_t resp;
    memcpy(req + 2, src, 8);
    return onewire_transfer(EEP_BUS, req, sizeof req, &resp, 1) == 1;
}

static bool eep_busy(void) {
    bool busy = true;
    return !eep_status(&busy) || busy;
}

// Joybus EEPROM reads carry no CRC, and a host's game erases whatever looks corrupt and writes that back.
// So the image is served only once two whole reads agree byte for byte; otherwise Maple sees no save at all.
static bool eep_read_stable(uint8_t *out, uint32_t size) {
    static uint8_t check[EEP_MAX];
    bool have = false;
    for (uint32_t t = 0; t < EEP_AGREE_TRIES; t++) {
        if (!eep_read(0, check, size)) continue;
        if (have && memcmp(out, check, size) == 0) return true;
        memcpy(out, check, size);
        have = true;
    }
    return false;
}

static bool eep_load(uint8_t *img, uint32_t size) {
    if (eep_read_stable(img, size)) return true;
    printf("eeprom: %lu bytes answered but no two reads agreed; Maple sees no save, so nothing is written\n",
           (unsigned long)size);
    return false;
}

static cart_save_chip_t g_chip = {
    .read = eep_read, .write = eep_write, .busy = eep_busy,
    .write_ms = EEP_WRITE_MS, .poll_us = EEP_POLL_US, .load = eep_load,
};

static bool g_bus;  // without the joybus SM there is no save, rather than transfers on an unclaimed SM

void cart_eeprom_init(void) {
    g_bus = onewire_init(EEP_BUS, PIN_S_DAT);
    if (!g_bus) {
        printf("eeprom: no joybus SM, no save\n");
        return;
    }
    gpio_pull_up(PIN_S_DAT);
    g_chip.size = eep_status(NULL);
    cart_sram_init(&g_chip);
}

void cart_eeprom_reload(void) {
    if (g_bus) g_chip.size = eep_status(NULL);
    cart_sram_reload();
}

// Finishes pending writes, then reads the chip itself, not the RAM copy. False if nothing answers.
static bool cart_eeprom_dump(void) {
    bool done = cart_sram_flush(2000);
    static uint8_t data[EEP_MAX];
    bool busy = false;
    uint32_t size = eep_status(&busy);
    if (!size) return false;
    cart_save_stats_t s = cart_sram_stats();
    printf("eeprom %lu bytes (%s Kbit)%s; %lu block writes, %lu verify fails, %lu write fails, %lu given up%s\n",
           (unsigned long)size, size == 512 ? "4" : "16", busy ? ", busy" : "", (unsigned long)s.writes,
           (unsigned long)s.verify_fails, (unsigned long)s.write_fails, (unsigned long)s.given_up,
           done ? "" : ", writes still pending");
    if (!eep_read(0, data, size)) {
        printf("eeprom read failed\n");
        return true;
    }
    console_hexdump(data, size);
    return true;
}

void cart_eeprom_diagnose(void) {
    if (cart_eeprom_dump()) return;
    printf("eeprom none answered on GP%d (cart pin 21); it also needs the clock on cart pin 19 (GP%d)\n",
           PIN_S_DAT, PIN_CIC_CLK);
    gpio_pull_down(PIN_S_DAT);
    sleep_us(200);
    bool high_vs_down = gpio_get(PIN_S_DAT);
    gpio_pull_up(PIN_S_DAT);
    sleep_us(200);
    bool high_vs_up = gpio_get(PIN_S_DAT);
    printf("S_DAT  %s\n", high_vs_down ? "high against the internal pull-down: an external pull-up is fitted"
                        : high_vs_up   ? "follows the internal pulls: only the ~50k internal pull-up holds it"
                                       : "low against the internal pull-up: held low or shorted");
}

bool cart_eeprom_write(uint32_t off, uint32_t len, const uint8_t *src) {
    return !((off | len) & 7u) && cart_sram_write(off, len, src);
}
