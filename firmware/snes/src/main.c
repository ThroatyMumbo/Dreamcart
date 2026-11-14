#include <stdio.h>
#include "pico/stdlib.h"
#include "boot.h"
#include "fault.h"
#include "snes_power.h"
#include "snes_bus.h"
#include "snes_cart.h"
#include "cart_sram.h"
#include "cart_stream.h"
#include "crc_dma.h"
#include "console.h"
#include "maple_cmds.h"
#include "commands.h"
#include "maple/dc_link.h"

#define SNES_CART_FUNC 0x00020000u  // ours; KOS reads 0x00000200

static const maple_devid_t g_cart_id = {
    .func          = SNES_CART_FUNC,
    .area_code     = 0xff,
    .connector_dir = 0x00,
    .name          = "SNES Cart Reader",
    .license       = "Produced By or Under License From SEGA ENTERPRISES,LTD.",
    .free_status   = "Version 1.000,2026/11/06,DreamcastCart sneslink",
    .standby_power = 100,
    .max_power     = 500,
};

// What the Maple cart unit serves, all called on core1; rom_span is set once the header is read.
static maple_cart_t g_cart = {
    .read        = cart_stream_read,
    .crc         = crc_dma,
    .save_size   = cart_sram_size,
    .save_read   = cart_sram_read,
    .save_write  = cart_sram_write,
    .save_status = cart_sram_status,
};

int main(void) {
    MARK(1);
    boot_init();
    MARK(2);

    snes_power_on();
    snes_bus_init();
    snes_cart_detect();
    MARK(7);

    static cart_save_chip_t chip = { .read = snes_cart_sram_read, .write = snes_cart_sram_write };
    chip.size = snes_cart()->sram_size;
    cart_sram_init(&chip);
    g_cart.rom_span = snes_cart()->rom_size;
    cart_stream_init(snes_cart_read, 0);
    crc_dma_init();
    bool maple_up = dc_link_start(&g_cart, &g_cart_id);
    MARK(9);
    boot_done();

    console_add(snes_commands, snes_ncommands);
    console_add(maple_commands, maple_ncommands);
    printf("\nsneslink ready, ? for help%s\n", maple_up ? "" : "  (maple failed to start)");
    snes_cart_print();
    for (;;) {
        cart_stream_service();
        cart_sram_service();
        console_poll();
    }
}
