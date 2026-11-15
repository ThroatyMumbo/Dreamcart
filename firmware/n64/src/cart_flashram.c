#include <stdio.h>
#include "pico/stdlib.h"
#include "board.h"
#include "n64_bus.h"
#include "console.h"
#include "cart_flashram.h"

#define FRAM_BYTES  131072u
#define BLOCK_BYTES N64_BUS_BURST

// Read-only by construction: erase and program only happen on D2 (execute), never sent here
#define FRAM_STATUS 0xE1000000u
#define FRAM_READ   0xF0000000u

static void fram_mode(uint32_t m) {
    if (m != FRAM_STATUS && m != FRAM_READ) return;
    const uint16_t words[2] = { (uint16_t)(m >> 16), (uint16_t)m };
    n64_bus_write_words(FRAM_CMD, words, 2);
    sleep_ms(10);
}

// id byte 7 from sanni's getFramType; the 1100 maps each 128-byte page onto 64 bytes of address
static const struct { uint8_t code; const char *name; bool halved; } FRAM_ID[] = {
    { 0x1E, "MX29L1100",                    true  },
    { 0x1D, "MX29L1101",                    false },
    { 0xF1, "MN63F81MPN",                   false },
    { 0x8E, "29L1100KC-15B0 (as MX29L1101)", false },
    { 0x84, "29L1100KC-15B0 (as MX29L1101)", false },
};

int cart_flashram_identify(void) {
    uint16_t w[4];
    fram_mode(FRAM_STATUS);
    n64_bus_read_words(CART_SAVE, w, 4);

    printf("fram   id %04X%04X %04X%04X\n", w[0], w[1], w[2], w[3]);
    if (w[0] != 0x1111 || w[1] != 0x8001) {
        printf("fram   no FlashRAM answered (expected 11118001); check /WRITE on cart pin 8\n");
        return -1;
    }
    for (unsigned i = 0; i < sizeof FRAM_ID / sizeof FRAM_ID[0]; i++) {
        if ((w[3] & 0xFF) == FRAM_ID[i].code) {
            printf("fram   %s\n", FRAM_ID[i].name);
            return (int)i;
        }
    }
    printf("fram   unknown device code %02X, not reading it\n", w[3] & 0xFF);
    return -1;
}

void cart_flashram_dump(void) {
    static uint8_t buf[BLOCK_BYTES];
    int t = cart_flashram_identify();
    if (t < 0) {
        printf("N64SAVE 0\n");
        return;
    }
    uint32_t chunk = FRAM_ID[t].halved ? 128 : BLOCK_BYTES;
    uint32_t step  = FRAM_ID[t].halved ? chunk / 2 : chunk;

    fram_mode(FRAM_READ);
    console_raw_begin("N64SAVE", FRAM_BYTES);
    for (uint32_t off = 0, addr = CART_SAVE; off < FRAM_BYTES; off += chunk, addr += step) {
        n64_bus_read(addr, buf, chunk);
        fwrite(buf, 1, chunk, stdout);
    }
    console_raw_end();
}
