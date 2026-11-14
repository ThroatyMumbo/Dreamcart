#include <stdio.h>
#include <string.h>
#include "hardware/gpio.h"
#include "board.h"
#include "console.h"
#include "snes_bus.h"
#include "snes_cart.h"
#include "cart_diag.h"

// Bus line -> cart edge pin (hardware/parts.py); bus bit k sits on U(k/8 + 1) output Q('A' + k%8)
static const uint8_t D_PIN[8] = { 19, 20, 21, 22, 50, 51, 52, 53 };
static const uint8_t ADDR_PIN[24] = { 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 37, 38, 39, 40,
                                      41, 42, 43, 44, 45, 46, 47, 48 };

static void data_pulls(bool up, bool down) {
    for (uint p = PIN_D0; p < PIN_D0 + 8; p++) gpio_set_pulls(p, up, down);
}

// A line the cart drives ignores the internal pull; an open one follows it.
void cart_diag_open_lines(void) {
    static uint8_t up[64], dn[64];
    uint8_t floating = 0;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t bus = (i & 1 ? 0xC00000u : 0) | (i / 2 * 8) << 16 | 0x8000u;
        data_pulls(true, false);
        snes_bus_read(bus, up, sizeof up);
        data_pulls(false, true);
        snes_bus_read(bus, dn, sizeof dn);
        for (unsigned j = 0; j < sizeof up; j++) floating |= up[j] ^ dn[j];
    }
    data_pulls(false, true);
    if (!floating) {
        printf("open lines: none, all 8 data lines are driven\n");
        return;
    }
    printf("open lines:\n");
    for (int b = 0; b < 8; b++)
        if (floating >> b & 1) printf("  D%d  GP%d  cart edge pin %d\n", b, PIN_D0 + b, D_PIN[b]);
}

static void name_lines(uint32_t diff) {
    for (int k = 0; k < 24; k++) {
        if (!(diff >> k & 1)) continue;
        printf(" %s%d (U%d Q%c, cart pin %d)", k < 16 ? "A" : "BA", k < 16 ? k : k - 16, k / 8 + 1, 'A' + k % 8,
               ADDR_PIN[k]);
    }
}

// An offset bit that never changes the data read back is not reaching the ROM.
void cart_diag_addr_lines(void) {
    const snes_cart_t *c = snes_cart();
    if (!c->rom_size) { printf("refused: no ROM size; 'i' first\n"); return; }
    static uint8_t a[32], b[32];
    const uint32_t base = 0x1000u;
    int bad = 0, top = 0;
    while (top < 23 && (2u << top) < c->rom_size) top++;
    for (int bit = 0; bit <= top; bit++) {
        uint32_t other = base ^ (1u << bit);
        snes_cart_read(base, a, sizeof a);
        snes_cart_read(other, b, sizeof b);
        if (memcmp(a, b, sizeof a)) continue;
        bool flat = true;
        for (unsigned i = 1; i < sizeof a; i++) if (a[i] != a[0]) { flat = false; break; }
        uint8_t s0, s1;
        uint32_t diff = snes_rom_bus(c, base, &s0) ^ snes_rom_bus(c, other, &s1);
        printf("  offset bit %-2d %s:", bit, flat ? "aliased (inconclusive, region is flat)" : "ALIASED");
        if (s0 != s1) printf(" chip register state");
        name_lines(diff);
        printf("\n");
        bad++;
    }
    if (!bad) printf("address lines: offset bits 0..%d all change the data\n", top);
}

void cart_diag_hexdump(uint32_t bus) {
    uint8_t d[64];
    snes_bus_read(bus, d, sizeof d);
    for (unsigned i = 0; i < sizeof d; i++) {
        if (i % 16 == 0) printf("\n%02lX:%04lX  ", (unsigned long)(bus >> 16 & 0xFFu), (unsigned long)((bus + i) & 0xFFFFu));
        printf("%02X ", d[i]);
    }
    printf("\n");
}

void cart_diag_dump(uint32_t kb) {
    uint32_t total = kb ? kb << 10 : snes_cart()->rom_size;
    if (!total || total > (8u << 20)) { printf("refused: 1-8192 KB, or a ROM size from the header\n"); return; }
    console_raw_dump("SNESDUMP", total, snes_cart_read);
}

void cart_diag_verify(void) {
    const snes_cart_t *c = snes_cart();
    if (!c->rom_size) { printf("refused: no ROM size; 'i' first\n"); return; }
    static uint32_t cs[SNES_EXHI_MAX / SNES_CHECK_CHUNK + 1];
    static uint8_t buf[512];
    memset(cs, 0, sizeof cs);
    for (uint32_t off = 0; off < c->rom_size; off += sizeof buf) {
        snes_cart_read(off, buf, sizeof buf);
        for (unsigned i = 0; i < sizeof buf; i++) cs[off / SNES_CHECK_CHUNK] += buf[i];
    }
    static const uint8_t MBIT[] = { 2, 4, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64 };
    printf("header checksum %04X; matching sizes:", c->checksum);
    int found = 0;
    for (unsigned i = 0; i < sizeof MBIT; i++) {
        uint32_t size = (uint32_t)MBIT[i] << 17;
        if (size > c->rom_size) break;
        if (snes_checksum(cs, size, c->chips) != c->checksum) continue;
        printf(" %u Mbit", MBIT[i]);
        found++;
    }
    if (found) printf("\n");
    else printf(" none (sum over %lu KB is %04X)\n", (unsigned long)(c->rom_size >> 10),
                snes_checksum(cs, c->rom_size, c->chips));
}
