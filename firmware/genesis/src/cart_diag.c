#include <stdio.h>
#include <string.h>
#include "hardware/gpio.h"
#include "board.h"
#include "console.h"
#include "md_bus.h"
#include "md_cart.h"
#include "cart_diag.h"

// Bus line -> cart pad (hardware/parts.py MD_VD_PIN, MD_VA_PIN); VAk sits on U((k-1)/8 + 1) Q('A' + (k-1)%8)
static const char *const VD_PAD[16] = { "A20", "A23", "A26", "A29", "A28", "A25", "A22", "A19",
                                        "A21", "A24", "A27", "A30", "B25", "B24", "B23", "B22" };
static const char *const VA_PAD[24] = { "", "A17", "A15", "A13", "A11", "A9", "A7", "A5", "A3", "B4", "B5", "A4",
                                        "A6", "A8", "A10", "A12", "A14", "A16", "B6", "B7", "B8", "B9", "B10", "B11" };

#define CHUNK 512u
#define SONIC2_PATCH_SUM 0x0635u    // OSCR MD.ino:131

static void data_pulls(bool up, bool down) {
    for (uint p = PIN_VD0; p < PIN_VD0 + 16; p++) gpio_set_pulls(p, up, down);
}

// A line the cart drives ignores the internal pull; an open one follows it.
void cart_diag_open_lines(void) {
    static uint8_t up[128], dn[128];
    uint16_t floating = 0;
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t word = i * 0x10000u;
        data_pulls(false, true);
        md_bus_read(word, dn, sizeof dn / 2);
        data_pulls(true, false);
        md_bus_read(word, up, sizeof up / 2);
        for (unsigned j = 0; j < sizeof up; j += 2)
            floating |= (uint16_t)((up[j] ^ dn[j]) << 8 | (up[j + 1] ^ dn[j + 1]));
    }
    if (!floating) {
        printf("open lines: none, all 16 data lines are driven\n");
        return;
    }
    printf("open lines:\n");
    for (int b = 0; b < 16; b++)
        if (floating >> b & 1) printf("  VD%d  GP%d  cart pad %s\n", b, PIN_VD0 + b, VD_PAD[b]);
}

static void name_lines(uint32_t diff) {
    for (int k = 1; k <= 23; k++)
        if (diff >> (k - 1) & 1) printf(" VA%d (U%d Q%c, cart pad %s)", k, (k - 1) / 8 + 1, 'A' + (k - 1) % 8, VA_PAD[k]);
}

// An offset bit that never changes the data read back is not reaching the ROM.
void cart_diag_addr_lines(void) {
    const md_cart_t *c = md_cart();
    if (!c->served) { printf("refused: no ROM size; 'i' first\n"); return; }
    static uint8_t a[32], b[32];
    const uint32_t base = 0x1000u;
    int bad = 0, top = 1;
    while (top < 23 && (2u << top) < c->served) top++;
    for (int bit = 1; bit <= top; bit++) {
        uint32_t other = base ^ (1u << bit);
        md_cart_read(base, a, sizeof a);
        md_cart_read(other, b, sizeof b);
        if (memcmp(a, b, sizeof a)) continue;
        bool flat = true;
        for (unsigned i = 1; i < sizeof a; i++) if (a[i] != a[0]) { flat = false; break; }
        uint8_t s0, s1;
        uint32_t diff = md_rom_bus(c, base, &s0) ^ md_rom_bus(c, other, &s1);
        printf("  offset bit %-2d %s:", bit, flat ? "aliased (inconclusive, region is flat)" : "ALIASED");
        if (s0 != s1) printf(" bank register");
        name_lines(diff);
        printf("\n");
        bad++;
    }
    if (!bad) printf("address lines: offset bits 1..%d all change the data\n", top);
}

void cart_diag_hexdump(uint32_t addr) {
    uint8_t d[64];
    addr &= 0xFFFFFEu;
    md_bus_read(addr >> 1, d, sizeof d / 2);
    for (unsigned i = 0; i < sizeof d; i++) {
        if (i % 16 == 0) printf("\n%06lX  ", (unsigned long)((addr + i) & 0xFFFFFFu));
        printf("%02X ", d[i]);
    }
    printf("\n");
}

void cart_diag_dump(uint32_t kb) {
    uint32_t total = kb ? kb << 10 : md_cart()->served;
    if (!total || total > MD_ROM_MAX) { printf("refused: 1-16384 KB, or a ROM size from the header\n"); return; }
    console_raw_dump("MDDUMP", total, md_cart_read);
}

static uint16_t sum_range(uint32_t from, uint32_t to) {
    static uint8_t buf[CHUNK];
    uint16_t sum = 0;
    for (uint32_t off = from; off < to; off += sizeof buf) {
        uint32_t n = to - off < sizeof buf ? to - off : sizeof buf;
        md_cart_read(off, buf, n);
        sum = md_sum_words(buf, n, sum);
    }
    return sum;
}

static void report(const char *part, uint16_t want, uint16_t got) {
    printf("%-8s checksum %04X, computed %04X  %s\n", part, want, got, want == got ? "OK" : "MISMATCH");
}

// OSCR MD.ino:1584-1713: each part skips its own first 256 words, except Sonic 2's patch
void cart_diag_verify(void) {
    const md_cart_t *c = md_cart();
    if (!c->served) { printf("refused: no ROM size; 'i' first\n"); return; }
    if (c->rom_size > 0x200) report("cart", c->checksum, sum_range(0x200, c->rom_size));
    if (c->snk < MD_SNK_S1 || c->snk > MD_SNK_OTHER) return;
    uint32_t lock_end = c->rom_size + c->lock_size;
    report("lock-on", c->lock_checksum, sum_range(c->rom_size + 0x200, lock_end));
    if (c->snk == MD_SNK_S2 && c->served > lock_end) report("patch", SONIC2_PATCH_SUM, sum_range(lock_end, c->served));
}
