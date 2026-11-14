#include <stdio.h>
#include <stdlib.h>
#include "md_bus.h"
#include "md_cart.h"
#include "cart_sram.h"
#include "cart_diag.h"
#include "commands.h"

static void cmd_info(const char *arg)       { md_cart_print(); }
static void cmd_hexdump(const char *arg)    { cart_diag_hexdump(*arg ? strtoul(arg, NULL, 16) : MD_HDR_BUS); }
static void cmd_dump(const char *arg)       { cart_diag_dump(*arg ? strtoul(arg, NULL, 0) : 0); }
static void cmd_verify(const char *arg)     { cart_diag_verify(); }
static void cmd_open_lines(const char *arg) { cart_diag_open_lines(); }
static void cmd_addr_lines(const char *arg) { cart_diag_addr_lines(); }
static void cmd_save_dump(const char *arg)  { cart_sram_dump("MDSAVE"); }

static void cmd_save(const char *arg) {
    const md_cart_t *c = md_cart();
    if (!md_save_served(c)) {
        printf("save none%s\n", c->save == MD_SAVE_EEPROM ? " (serial EEPROM unsupported)"
                                : c->save == MD_SAVE_UNKNOWN ? " (unknown layout, see 'i')" : "");
        return;
    }
    printf("save %lu bytes, %s, at %06lX\n", (unsigned long)c->save_size, md_save_name(c->save),
           (unsigned long)md_save_bus(c));
    cart_sram_diagnose();
}

static void cmd_bus_hz(const char *arg) {
    if (*arg && !md_bus_set_hz(strtoul(arg, NULL, 0) * 1000u))
        printf("refused: %u-%u kHz\n", MD_BUS_HZ_MIN / 1000u, MD_BUS_HZ_MAX / 1000u);
    printf("bus clock %lu kHz, SRCLK %lu kHz\n", (unsigned long)(md_bus_hz() / 1000u),
           (unsigned long)(md_bus_hz() / 2000u));
}

const console_cmd_t md_commands[] = {
    { 'i', "i          header + identify", cmd_info },
    { 'x', "x [hex]    hexdump 64 bytes at a byte address, default 000100", cmd_hexdump },
    { 'd', "d [kb]     raw ROM dump in .bin order (default the served size)", cmd_dump },
    { 'v', "v          checksum the ROM against the header, each lock-on part too", cmd_verify },
    { 'o', "o          open data line test (pull up vs pull down)", cmd_open_lines },
    { 'a', "a          address line aliasing test", cmd_addr_lines },
    { 'e', "e          save: size, stats, hexdump from the chip, read-only", cmd_save },
    { 'r', "r          raw save dump from the chip, read-only", cmd_save_dump },
    { 'c', "c [khz]    cart bus PIO clock, default 20000", cmd_bus_hz },
};

const unsigned md_ncommands = sizeof md_commands / sizeof md_commands[0];
