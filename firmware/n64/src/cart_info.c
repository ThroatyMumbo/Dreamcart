#include <stdio.h>
#include <string.h>
#include "board.h"
#include "n64_bus.h"
#include "cart_info.h"

static const struct { const char *name; uint32_t crc1; uint8_t mb; } KNOWN[] = {
    { "Pokemon Stadium 2 (USA)",    0x03571182, 64 },
    { "Pokemon Stadium 2 (Europe)", 0x2952369C, 64 },
    { "Pokemon Stadium 2 (France)", 0xAC5AA5C7, 64 },
    { "Pokemon Stadium 2 (Germany)", 0x439B7E7E, 64 },
    { "Pokemon Stadium 2 (Italy)",  0xEFCEAF00, 64 },
    { "Pokemon Stadium 2 (Spain)",  0xD0A1FC5B, 64 },
    { "Pokemon Stadium 2 (Japan)",  0x63775886, 32 },
    { "Tony Hawk's Pro Skater 2 (USA)", 0x99150E18, 16 },
};

static uint32_t g_rom_mb = ROM_MB_MAX;

uint32_t cart_rom_mb(void) { return g_rom_mb; }
uint32_t cart_rom_size(void) { return g_rom_mb << 20; }

bool cart_set_rom_mb(uint32_t mb) {
    if (mb < 1 || mb > ROM_MB_MAX) return false;
    g_rom_mb = mb;
    return true;
}

void cart_header_read(uint8_t h[CART_HDR_LEN]) { n64_bus_read(CART_ROM, h, CART_HDR_LEN); }

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint32_t cart_header_magic(const uint8_t *h) { return be32(h); }

void cart_header_title(const uint8_t *h, char title[21]) {
    memcpy(title, h + 0x20, 20);
    title[20] = 0;
    for (int i = 19; i >= 0 && title[i] == ' '; i--) title[i] = 0;
}

void cart_info_print(void) {
    uint8_t h[CART_HDR_LEN];
    cart_header_read(h);

    printf("header:");
    for (int i = 0; i < 64; i++) {
        if (i % 16 == 0) printf("\n  %02X  ", i);
        printf("%02X ", h[i]);
    }
    printf("\n\n");

    uint32_t magic = cart_header_magic(h);
    printf("magic  %08lX  ", (unsigned long)magic);
    if (magic == N64_MAGIC) {
        printf("OK\n");
    } else if (magic == 0x37804012u) {
        printf("BYTE-SWAPPED: AD0-7 and AD8-15 are swapped at the connector\n");
    } else if (magic == 0x40123780u) {
        printf("WORD-SWAPPED\n");
    } else {
        printf("BAD (expected 80371240)\n");
    }

    uint32_t crc1 = be32(h + 0x10);
    printf("CRC1   %08lX\n", (unsigned long)crc1);
    printf("CRC2   %08lX\n", (unsigned long)be32(h + 0x14));

    char title[21];
    cart_header_title(h, title);
    printf("title  \"%s\"\n", title);
    printf("cartID %c%c%c%c rev %02X\n", h[0x3B], h[0x3C], h[0x3D], h[0x3E], h[0x3F]);

    for (unsigned i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++) {
        if (KNOWN[i].crc1 == crc1) {
            g_rom_mb = KNOWN[i].mb;
            printf("match  %s, %u MB\n", KNOWN[i].name, (unsigned)KNOWN[i].mb);
            return;
        }
    }
    printf("match  none in table\n");
}
