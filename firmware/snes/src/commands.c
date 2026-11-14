#include <stdio.h>
#include <stdlib.h>
#include "snes_bus.h"
#include "snes_cart.h"
#include "cart_sram.h"
#include "cart_diag.h"
#include "commands.h"

static void cmd_info(const char *arg)       { snes_cart_print(); }
static void cmd_hexdump(const char *arg)    { cart_diag_hexdump(*arg ? strtoul(arg, NULL, 16) : 0x00FFC0u); }
static void cmd_dump(const char *arg)       { cart_diag_dump(*arg ? strtoul(arg, NULL, 0) : 0); }
static void cmd_verify(const char *arg)     { cart_diag_verify(); }
static void cmd_open_lines(const char *arg) { cart_diag_open_lines(); }
static void cmd_addr_lines(const char *arg) { cart_diag_addr_lines(); }
static void cmd_save_dump(const char *arg)  { cart_sram_dump("SNESSAVE"); }

static void cmd_save(const char *arg) {
    const snes_cart_t *c = snes_cart();
    if (!c->sram_size) {
        printf("save none%s\n", c->chip == SNES_CHIP_SA1 ? " (SA-1 unsupported)" : c->battery ? "" : ": no battery");
        return;
    }
    printf("save %lu bytes at %06lX\n", (unsigned long)c->sram_size, (unsigned long)snes_sram_bus(c, 0));
    cart_sram_diagnose();
}

static void cmd_bus_hz(const char *arg) {
    if (*arg && !snes_bus_set_hz(strtoul(arg, NULL, 0) * 1000u))
        printf("refused: %u-%u kHz\n", SNES_BUS_HZ_MIN / 1000u, SNES_BUS_HZ_MAX / 1000u);
    printf("bus clock %lu kHz, SRCLK %lu kHz\n", (unsigned long)(snes_bus_hz() / 1000u),
           (unsigned long)(snes_bus_hz() / 2000u));
}

const console_cmd_t snes_commands[] = {
    { 'i', "i          header + identify", cmd_info },
    { 'x', "x [hex]    hexdump 64 bytes at a 24-bit bus address, default 00FFC0", cmd_hexdump },
    { 'd', "d [kb]     raw ROM dump in file order (default the served size)", cmd_dump },
    { 'v', "v          checksum the ROM and report which real sizes match the header", cmd_verify },
    { 'o', "o          open data line test (pull up vs pull down)", cmd_open_lines },
    { 'a', "a          address line aliasing test", cmd_addr_lines },
    { 'e', "e          save: size, stats, hexdump from the chip, read-only", cmd_save },
    { 'r', "r          raw save dump from the chip, read-only", cmd_save_dump },
    { 'c', "c [khz]    cart bus PIO clock, default 20000", cmd_bus_hz },
};

const unsigned snes_ncommands = sizeof snes_commands / sizeof snes_commands[0];
