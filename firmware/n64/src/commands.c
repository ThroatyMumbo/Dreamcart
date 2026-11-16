#include <stdio.h>
#include <stdlib.h>
#include "board.h"
#include "n64_bus.h"
#include "cart_info.h"
#include "cart_diag.h"
#include "cart_flashram.h"
#include "cart_eeprom.h"
#include "cart_sram.h"
#include "cic.h"
#include "cart_power.h"
#include "cart_pass.h"
#include "pico/time.h"
#include "commands.h"

static void cmd_info(const char *arg)       { cart_info_print(); }
static void cmd_open_lines(const char *arg) { cart_diag_open_lines(); }
static void cmd_stuck(const char *arg)      { cart_diag_stuck(*arg ? strtoul(arg, NULL, 0) : 1024); }
static void cmd_addr_lines(const char *arg) { cart_diag_addr_lines(); }
static void cmd_hexdump(const char *arg)    { cart_diag_hexdump(*arg ? strtoul(arg, NULL, 16) : CART_ROM); }
static void cmd_fram_id(const char *arg)    { cart_flashram_identify(); }
static void cmd_fram_dump(const char *arg)  { cart_flashram_dump(); }

// The console's whole-ROM pass, run here; the starts count is the same every run while the ROM reads true
static void cmd_pass(const char *arg) {
    uint32_t w[4], t0 = time_us_32();
    if (!cart_pass_sync(CART_SYNC_PASS, 0, 0)) { printf("refused: a pass or commit is running\n"); return; }
    do {
        cart_pass_service();
        cart_pass_info(w);
    } while (w[CART_INFO(N64_MI_PASS)] == PASS_RUNNING);
    printf("pass %s in %lu ms, %lu starts\n", w[CART_INFO(N64_MI_PASS)] == PASS_DONE ? "done" : "failed",
           (unsigned long)((time_us_32() - t0) / 1000), (unsigned long)w[CART_INFO(N64_MI_STARTS)]);
}
static void cmd_cic(const char *arg)        { cic_cmd(); }
static void cmd_size(const char *arg)       { cart_diag_size(); }
static void cmd_eeprom(const char *arg)     { cart_eeprom_diagnose(); }
static void cmd_swap(const char *arg)       { cart_swap(); }

// One 8-byte EEPROM block from the console; the main loop writes it to the chip and reads it back
static void cmd_eeprom_put(const char *arg) {
    char *end;
    unsigned long block = strtoul(arg, &end, 0);
    while (*end == ' ') end++;
    uint8_t data[8];
    for (int i = 0; i < 8; i++) {
        char hex[3] = { end[2 * i], end[2 * i + 1], 0 };
        char *stop;
        data[i] = (uint8_t)strtoul(hex, &stop, 16);
        if (!hex[0] || !hex[1] || *stop) {
            printf("refused: p <block> <16 hex digits>\n");
            return;
        }
    }
    if (block >= cart_sram_size() / 8 || !cart_eeprom_write((uint32_t)block * 8, 8, data)) {
        printf("refused: block %lu, the EEPROM serves %lu bytes\n", block, (unsigned long)cart_sram_size());
        return;
    }
    printf("eeprom block %lu queued\n", block);
}

static void cmd_dump(const char *arg) {
    uint32_t mb = *arg ? strtoul(arg, NULL, 0) : cart_rom_mb();
    if (mb < 1 || mb > ROM_MB_MAX) printf("refused: 1-%u MB\n", ROM_MB_MAX);
    else cart_diag_dump(mb);
}

static void cmd_cic_half(const char *arg) {
    if (*arg && !cic_set_half_us(strtoul(arg, NULL, 0))) printf("refused: 1-%u us\n", CIC_HALF_US_MAX);
    printf("CIC half-period %lu us\n", (unsigned long)cic_half_us());
}

static void cmd_rom_mb(const char *arg) {
    if (*arg && !cart_set_rom_mb(strtoul(arg, NULL, 0))) printf("refused: 1-%u MB\n", ROM_MB_MAX);
    printf("rom size %lu MB\n", (unsigned long)cart_rom_mb());
}

static void cmd_write_feed(const char *arg) {
    printf("bus writes fed by %s\n", n64_bus_toggle_write_cpu() ? "CPU" : "DMA");
}

const console_cmd_t n64_commands[] = {
    { 'i', "i          header + identify", cmd_info },
    { 'o', "o          open AD line test (pull up vs pull down)", cmd_open_lines },
    { 's', "s [n]      stuck data bit scan, n sample blocks (default 1024)", cmd_stuck },
    { 'a', "a          address line aliasing test", cmd_addr_lines },
    { 'x', "x <hex>    hexdump 64 bytes at address", cmd_hexdump },
    { 'd', "d [mb]     raw binary dump (default: ROM size)", cmd_dump },
    { 'f', "f          FlashRAM id", cmd_fram_id },
    { 'r', "r          raw FlashRAM save dump, 128 KB, read-only", cmd_fram_dump },
    { 'k', "k          reset the CIC and read the handshake: region, seed, variant", cmd_cic },
    { 'y', "y [us]     first DCLK half period k tries, default 100", cmd_cic_half },
    { 'm', "m <mb>     set ROM size, default 64, bounds s and a", cmd_rom_mb },
    { 'z', "z          probe ROM size", cmd_size },
    { 'e', "e          EEPROM type + hexdump, read-only", cmd_eeprom },
    { 'p', "p <n> <hex> write EEPROM block n (16 hex digits), verified on the chip", cmd_eeprom_put },
    { 'w', "w          toggle FlashRAM command writes between DMA and CPU feed", cmd_write_feed },
    { 'h', "h          hot swap: cart power off, Enter after the swap powers the new one up", cmd_swap },
    { 'q', "q          the console's whole-ROM pass, timed, with its starts count", cmd_pass },
};

const unsigned n64_ncommands = sizeof n64_commands / sizeof n64_commands[0];
