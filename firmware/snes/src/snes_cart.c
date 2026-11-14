#include <stdio.h>
#include <string.h>
#include "console.h"
#include "snes_bus.h"
#include "snes_cart.h"

#define PIECE 0x2000u   // every mapping is linear inside an aligned 8 KB of offsets

static snes_cart_t g_cart;
static int g_state = -1;    // chip register state last written; -1 unknown

static void apply(const snes_reg_write_t *w, unsigned n) {
    for (unsigned i = 0; i < n; i++) snes_bus_write(w[i].bus, &w[i].val, 1);
}

static void enter(uint8_t state) {
    if (state == SNES_STATE_ANY || state == g_state) return;
    snes_reg_write_t w[2];
    apply(w, snes_state_writes(&g_cart, state, w));
    g_state = state;
}

void snes_cart_detect(void) {
    uint8_t h[SNES_HDR_LEN];
    snes_bus_read(SNES_HDR_BUS, h, sizeof h);
    snes_parse_header(h, &g_cart);
    g_state = -1;
}

const snes_cart_t *snes_cart(void) { return &g_cart; }

void snes_cart_read(uint32_t off, void *dst, uint32_t n) {
    uint8_t *d = dst;
    if (!g_cart.rom_size) { memset(d, 0xFF, n); return; }
    while (n) {
        uint32_t o = off % g_cart.rom_size;
        uint32_t k = PIECE - o % PIECE;
        if (k > n) k = n;
        uint8_t state;
        uint32_t bus = snes_rom_bus(&g_cart, o, &state);
        enter(state);
        snes_bus_read(bus, d, k);
        off += k;
        d += k;
        n -= k;
    }
}

static void sram_xfer(uint32_t off, uint8_t *dst, const uint8_t *src, uint32_t n) {
    snes_reg_write_t w[1];
    apply(w, snes_sram_enable_writes(&g_cart, true, w));
    while (n) {
        uint32_t k = PIECE - off % PIECE;
        if (k > n) k = n;
        uint32_t bus = snes_sram_bus(&g_cart, off);
        if (dst) { snes_bus_read(bus, dst, k); dst += k; }
        else     { snes_bus_write(bus, src, k); src += k; }
        off += k;
        n -= k;
    }
    apply(w, snes_sram_enable_writes(&g_cart, false, w));
}

bool snes_cart_sram_read(uint32_t off, uint8_t *dst, uint32_t n) {
    sram_xfer(off, dst, NULL, n);
    return true;
}

bool snes_cart_sram_write(uint32_t off, const uint8_t *src, uint32_t n) {
    sram_xfer(off, NULL, src, n);
    return true;
}

void snes_cart_print(void) {
    const snes_cart_t *c = &g_cart;
    printf("header at 00:FFB0:");
    for (unsigned i = 0; i < SNES_HDR_LEN; i++) {
        if (i % 16 == 0) printf("\n  %04X  ", (unsigned)(0xFFB0 + i));
        printf("%02X ", c->hdr[i]);
    }
    printf("\n\ntitle    \"%s\"\n", c->title);
    printf("map      %02X  %s\n", c->map_byte, snes_map_name(c->map));
    printf("chips    %02X  %s%s\n", c->chips, snes_chip_name(c->chip), c->battery ? ", battery" : "");
    printf("checksum %04X complement %04X  %s\n", c->checksum, c->complement,
           c->header_ok ? "OK" : "BAD: header may be misread, check 'o' and 'a'");
    if (c->size_ok) console_print_size("rom      declared ", c->rom_declared);
    else printf("rom      size byte %02X out of range", c->hdr[0xFFD7 - SNES_HDR_BUS]);
    if (c->rom_size) console_print_size(", serving ", c->rom_size);
    printf("\n");
    if (c->sram_size) {
        console_print_size("save     ", c->sram_size);
        printf(" at %02lX:%04lX\n", (unsigned long)(snes_sram_bus(c, 0) >> 16),
               (unsigned long)(snes_sram_bus(c, 0) & 0xFFFFu));
    } else {
        printf("save     none\n");
    }
    if (c->chip == SNES_CHIP_SA1) printf("SA-1 is not supported: no ROM and no save over Maple\n");
    else if (!c->rom_size) printf("no usable ROM size: Maple serves nothing\n");
}
