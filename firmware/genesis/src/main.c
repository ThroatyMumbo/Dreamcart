#include <stdio.h>
#include "pico/stdlib.h"
#include "boot.h"
#include "fault.h"
#include "md_power.h"
#include "md_bus.h"
#include "md_cart.h"
#include "cart_sram.h"
#include "cart_stream.h"
#include "crc_dma.h"
#include "console.h"
#include "maple_cmds.h"
#include "commands.h"
#include "maple/dc_link.h"

#define MD_CART_FUNC 0x00040000u    // ours; KOS reads 0x00000400

static const maple_devid_t g_cart_id = {
    .func          = MD_CART_FUNC,
    .area_code     = 0xff,
    .connector_dir = 0x00,
    .name          = "Mega Drive Cart Reader",
    .license       = "Produced By or Under License From SEGA ENTERPRISES,LTD.",
    .free_status   = "Version 1.000,2026/11/06,DreamcastCart mdlink",
    .standby_power = 100,
    .max_power     = 500,
};

// GET_MEDIA_INFO words 6-8: md_save_t, the save's first bus byte, and 1 when A130F1 switches it over ROM.
static uint32_t g_info[MAPLE_CART_INFO_WORDS];

static void __not_in_flash_func(md_info)(uint32_t *w) {
    for (unsigned i = 0; i < MAPLE_CART_INFO_WORDS; i++) w[i] = g_info[i];
}

// What the Maple cart unit serves, all called on core1; rom_span is set once the header is read.
static maple_cart_t g_cart = {
    .read        = cart_stream_read,
    .crc         = crc_dma,
    .save_size   = cart_sram_size,
    .save_read   = cart_sram_read,
    .save_write  = cart_sram_write,
    .save_status = cart_sram_status,
    .info        = md_info,
};

int main(void) {
    MARK(1);
    boot_init();
    MARK(2);

    md_power_on();
    md_bus_init();
    md_cart_detect();
    MARK(7);

    static cart_save_chip_t chip = { .read = md_cart_save_read, .write = md_cart_save_write };
    if (md_save_served(md_cart())) {
        chip.size = md_cart()->save_size;
        g_info[0] = md_cart()->save;
        g_info[1] = md_save_bus(md_cart());
        g_info[2] = md_cart()->sram_switch;
    }
    cart_sram_init(&chip);
    g_cart.rom_span = md_cart()->served;
    cart_stream_init(md_cart_read, 0);
    crc_dma_init();
    bool maple_up = dc_link_start(&g_cart, &g_cart_id);
    MARK(9);
    boot_done();

    console_add(md_commands, md_ncommands);
    console_add(maple_commands, maple_ncommands);
    printf("\nmdlink ready, ? for help%s\n", maple_up ? "" : "  (maple failed to start)");
    md_cart_print();
    for (;;) {
        cart_stream_service();
        cart_sram_service();
        console_poll();
    }
}
