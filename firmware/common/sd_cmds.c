#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "psram.h"
#include "sd.h"
#include "sd_cmds.h"

#define SPEED_RUN 64u                   // blocks per CMD18

static uint32_t g_sector[128];

static const char *fail_text(uint32_t f) {
    return f & SD_FAIL_NO_RESPONSE ? "no response" : f & SD_FAIL_RESPONSE ? "bad response"
         : f & SD_FAIL_NO_DATA ? "no data block" : f & SD_FAIL_DATA_CRC ? "data CRC error"
         : f & SD_FAIL_OVERRUN ? "CRC check overrun" : "rejected";
}

static void print_failure(const char *who, uint32_t f) {
    printf("%-6s failed at CMD%lu: %s, last response %08lX\n", who, (unsigned long)(f & 0xFF), fail_text(f),
           (unsigned long)sd_stats.resp);
}

static void print_stats(void) {
    printf("stats  reads %lu  crc errors %lu  timeouts %lu  cmd errors %lu  overruns %lu\n",
           (unsigned long)sd_stats.reads, (unsigned long)sd_stats.crc_errors, (unsigned long)sd_stats.timeouts,
           (unsigned long)sd_stats.cmd_errors, (unsigned long)sd_stats.overruns);
}

static bool card_up(void) {
    if (sd_card()) return true;
    if (sd_init(SD_MAX_HZ, false)) return true;
    print_failure("sd", sd_stats.fail);
    return false;
}

static void cmd_card(const char *arg) {
    char *end;
    uint32_t mhz = strtoul(arg, &end, 0);
    if (end == arg) mhz = SD_MAX_HZ / 1000000u;
    while (*end == ' ') end++;
    bool force_spi = strncmp(end, "spi", 3) == 0;
    printf("sd     %s\n", sd_present() ? "card detect high: card in the slot" : "card detect low: no card");
    bool up = sd_init(mhz * 1000000u, force_spi);
    if (sd_stats.sdio_fail) print_failure("sdio", sd_stats.sdio_fail);
    if (!up) {
        print_failure(force_spi || sd_stats.sdio_fail ? "spi" : "sd", sd_stats.fail);
        print_stats();
        return;
    }
    const sd_card_t *c = sd_card();
    const uint8_t *id = c->cid;
    printf("sd     %s at %lu.%03lu MHz, %s speed; CMD6 group 1 support %04X\n",
           c->mode == SD_MODE_SDIO ? "SDIO 4-bit" : "SPI", (unsigned long)(c->hz / 1000000u),
           (unsigned long)(c->hz / 1000u % 1000u), c->high_speed ? "high" : "default", c->hs_support);
    printf("sd     %s, OCR %08lX, %lu MB (%lu sectors)\n", c->block_addr ? "SDHC/SDXC" : "SDSC",
           (unsigned long)c->ocr, (unsigned long)(c->sectors / 2048u), (unsigned long)c->sectors);
    uint32_t mdt = (uint32_t)(id[13] & 0x0F) << 8 | id[14];
    printf("cid    mid %02X oem %c%c product %c%c%c%c%c rev %u.%u serial %02X%02X%02X%02X made %lu-%02lu\n",
           id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8] >> 4, id[8] & 0x0F,
           id[9], id[10], id[11], id[12], (unsigned long)(2000 + (mdt >> 4)), (unsigned long)(mdt & 0x0F));
    if (c->mode == SD_MODE_SPI) printf("sd     the card stays in SPI mode until it is reinserted\n");
    print_stats();
}

static void cmd_sector(const char *arg) {
    uint32_t lba = *arg ? strtoul(arg, NULL, 0) : 0;
    if (!card_up()) return;
    if (!sd_read(lba, 1, (uint8_t *)g_sector)) {
        print_failure("sd", sd_stats.fail);
        return;
    }
    const uint8_t *b = (const uint8_t *)g_sector;
    for (int i = 0; i < 512; i++) {
        if (i % 16 == 0) printf("\n%03X  ", i);
        printf("%02X ", b[i]);
    }
    printf("\n");
}

static void cmd_speed(const char *arg) {
    uint32_t mb = *arg ? strtoul(arg, NULL, 0) : 4;
    if (!card_up()) return;
    uint32_t runs = mb * 2048u / SPEED_RUN, failed = 0;
    if (runs == 0 || (uint64_t)runs * SPEED_RUN > sd_card()->sectors) {
        printf("refused: 1-%lu MB\n", (unsigned long)(sd_card()->sectors / 2048u));
        return;
    }
    sd_stats_t before = sd_stats;
    sd_stats.sum = 0;
    uint64_t t0 = time_us_64();
    for (uint32_t k = 0; k < runs; k++)
        if (!sd_read(k * SPEED_RUN, SPEED_RUN, NULL)) failed++;
    uint32_t us = (uint32_t)(time_us_64() - t0);
    uint32_t kbs = (uint32_t)((uint64_t)runs * SPEED_RUN * 512u * 1000u / us);
    printf("speed  %lu MB in %lu ms: %lu.%03lu MB/s, %lu us per %u-block run, sum %08lX\n", (unsigned long)mb,
           (unsigned long)(us / 1000u), (unsigned long)(kbs / 1000u), (unsigned long)(kbs % 1000u),
           (unsigned long)(us / runs), SPEED_RUN, (unsigned long)sd_stats.sum);
    printf("speed  %lu of %lu runs failed, %lu CRC errors, %lu timeouts, %lu overruns\n", (unsigned long)failed,
           (unsigned long)runs, (unsigned long)(sd_stats.crc_errors - before.crc_errors),
           (unsigned long)(sd_stats.timeouts - before.timeouts), (unsigned long)(sd_stats.overruns - before.overruns));
    if (failed) print_failure("sd", sd_stats.fail);
}

// A pattern over the first kb of PSRAM, written then read back through the XIP window, with both rates
static void cmd_psram(const char *arg) {
    uint32_t kb = strtoul(arg, NULL, 0), size = psram_size();
    if (!size) { printf("psram  none found\n"); return; }
    if (!kb || kb > size >> 10) kb = size >> 10;
    volatile uint32_t *p = (volatile uint32_t *)PSRAM_BASE;
    uint32_t words = kb * 256, bad = 0, t0 = time_us_32();
    for (uint32_t i = 0; i < words; i++) p[i] = i * 0x9E3779B9u;
    uint32_t t1 = time_us_32();
    for (uint32_t i = 0; i < words; i++) bad += p[i] != i * 0x9E3779B9u;
    uint32_t t2 = time_us_32();
    printf("psram  %lu KB of %lu: %lu bad words, write %lu KB/s, read %lu KB/s (cached)\n", (unsigned long)kb,
           (unsigned long)(size >> 10), (unsigned long)bad, (unsigned long)(kb * 1000000ull / (t1 - t0 + 1)),
           (unsigned long)(kb * 1000000ull / (t2 - t1 + 1)));
}

const console_cmd_t sd_commands[] = {
    { 'l', "l [kb]     PSRAM pattern write and read-back (default all of it)", cmd_psram },
    { 'c', "c [mhz] [spi] microSD init: SDIO, else SPI (default 50 MHz; above 25 needs high speed)", cmd_card },
    { 'g', "g [lba]    hexdump one microSD sector (default 0)", cmd_sector },
    { 'v', "v [mb]     microSD read speed, CMD18 runs from sector 0 (default 4 MB), with a word sum", cmd_speed },
};

const unsigned sd_ncommands = sizeof sd_commands / sizeof sd_commands[0];
