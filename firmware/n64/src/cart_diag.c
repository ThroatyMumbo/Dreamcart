#include <stdio.h>
#include <string.h>
#include "hardware/gpio.h"
#include "board.h"
#include "n64_bus.h"
#include "cart_info.h"
#include "console.h"
#include "cart_diag.h"

// AD line -> cart edge finger, for naming a fault in terms you can probe
static const uint8_t AD_EDGE_PIN[16] = { 28, 29, 30, 32, 36, 37, 40, 41,
                                         16, 15, 12, 11, 7, 5, 4, 3 };

static void ad_pulls(bool up, bool down) {
    for (int i = 0; i < 16; i++) gpio_set_pulls(AD_BASE + i, up, down);
}

// A line the ROM is actually driving ignores the internal pull; an open one follows it.
// The last 16 words of a 64-word read come 20+ us after the latch, long enough for the pull to win.
static uint16_t float_mask(uint32_t addr) {
    static uint16_t up[64], dn[64];
    ad_pulls(true, false);
    n64_bus_read_words(addr, up, 64);
    ad_pulls(false, true);
    n64_bus_read_words(addr, dn, 64);
    ad_pulls(false, false);
    uint16_t mask = 0xFFFFu;
    for (int i = 48; i < 64; i++) mask &= (uint16_t)(up[i] ^ dn[i]);
    return mask;
}

void cart_diag_open_lines(void) {
    const uint32_t samples = 64;
    uint16_t floating = 0;

    for (uint32_t i = 0; i < samples; i++) floating |= float_mask(CART_ROM + i * 0x10000u);

    if (!floating) {
        printf("open lines: none, all 16 AD lines are driven\n");
        return;
    }
    printf("open lines:\n");
    for (int b = 0; b < 16; b++) {
        if (floating & (1u << b))
            printf("  AD%-2d  GP%-2d  cart edge pin %d\n", b, AD_BASE + b, AD_EDGE_PIN[b]);
    }
}

void cart_diag_stuck(uint32_t samples) {
    uint16_t ones = 0, zeros = 0;
    uint16_t w[64];
    uint32_t step = cart_rom_bytes() / (samples ? samples : 1);
    step &= ~127u;
    if (step < 128) step = 128;

    for (uint32_t off = 0; off < cart_rom_bytes(); off += step) {
        n64_bus_read_words(CART_ROM + off, w, 64);
        for (int i = 0; i < 64; i++) { ones |= w[i]; zeros |= (uint16_t)~w[i]; }
    }

    uint16_t bad = (uint16_t)~(ones & zeros);
    if (!bad) {
        printf("stuck bits: none, all 16 data bits toggled\n");
        return;
    }
    printf("stuck bits:\n");
    for (int b = 0; b < 16; b++) {
        if (!(bad & (1u << b))) continue;
        printf("  AD%-2d  GP%-2d  cart edge pin %-2d  always %d\n",
               b, AD_BASE + b, AD_EDGE_PIN[b], (ones >> b) & 1);
    }
}

// An address bit that never changes the data read back is not reaching the ROM
void cart_diag_addr_lines(void) {
    static uint16_t a[64], b[64];
    const uint32_t base = CART_ROM + 0x2000u;
    int bad = 0;

    // bits above the top of the ROM alias legitimately, so stop at the real size
    int top = 1;
    while (top < 30 && (1u << (top + 1)) < cart_rom_bytes()) top++;

    for (int bit = 1; bit <= top; bit++) {
        n64_bus_read_words(base, a, 64);
        n64_bus_read_words(base ^ (1u << bit), b, 64);

        bool flat = true;
        for (int i = 1; i < 64; i++) if (a[i] != a[0]) { flat = false; break; }

        if (memcmp(a, b, sizeof a) == 0) {
            printf("  A%-2d  %s\n", bit, flat ? "aliased (inconclusive, region is flat)"
                                              : "ALIASED - address bit not reaching the ROM");
            bad++;
        }
    }
    if (!bad) printf("address lines: all of A1..A%d change the data\n", top);
}

// the ROM ends at the first 1 MB step where every AD line floats, or the boot code mirrors
uint32_t cart_diag_probe_size(void) {
    static uint16_t base[64], probe[64];
    n64_bus_read_words(CART_ROM + 0x40, base, 64);
    for (uint32_t mb = 1; mb < ROM_MB_MAX; mb++) {
        uint32_t off = mb << 20;
        if (float_mask(CART_ROM + off) == 0xFFFFu) return off;
        n64_bus_read_words(CART_ROM + off + 0x40, probe, 64);
        if (memcmp(base, probe, sizeof base) == 0) return off;
    }
    return ROM_LIMIT;
}

void cart_diag_size(void) {
    uint32_t size = cart_diag_probe_size();
    cart_set_rom_mb(size >> 20);
    printf("rom size %lu MB (%lu bytes), now bounds s, a and d\n", (unsigned long)cart_rom_mb(), (unsigned long)size);
}

void cart_diag_hexdump(uint32_t addr) {
    uint16_t w[32];
    n64_bus_read_words(addr, w, 32);
    for (int i = 0; i < 32; i++) {
        if (i % 8 == 0) printf("\n%08lX  ", (unsigned long)(addr + i * 2));
        printf("%04X ", w[i]);
    }
    printf("\n");
}

static void rom_read(uint32_t off, void *dst, uint32_t n) { n64_bus_read(CART_ROM + off, dst, n); }

void cart_diag_dump(uint32_t mb) { console_raw_dump("N64DUMP", mb * 1024u * 1024u, rom_read); }
