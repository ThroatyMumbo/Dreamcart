#include <stdio.h>
#include "pico/stdlib.h"
#include "board.h"
#include "boot.h"
#include "fault.h"
#include "n64_bus.h"
#include "cic.h"
#include "cart_diag.h"
#include "cart_eeprom.h"
#include "cart_flashram.h"
#include "cart_sram.h"
#include "cart_info.h"
#include "cart_pass.h"
#include "cart_power.h"
#include "cart_stream.h"
#include "crc_dma.h"
#include "console.h"
#include "maple_cmds.h"
#include "psram.h"
#include "sd.h"
#include "sd_cmds.h"
#include "xcache.h"
#include "commands.h"
#include "maple/dc_link.h"
#include "cart_sync.h"

static const maple_devid_t g_cart_id = {
    .func          = N64_CART_FUNC,
    .area_code     = 0xff,
    .connector_dir = 0x00,
    .name          = "N64 Cart Reader",
    .license       = "Produced By or Under License From SEGA ENTERPRISES,LTD.",
    .free_status   = "Version 1.000,2026/11/04,DreamcastCart n64link",
    .standby_power = 100,
    .max_power     = 500,
};

static bool __not_in_flash_func(n64_sync)(uint32_t op, uint32_t a, uint32_t b) {
    switch (op) {
    case CART_SYNC_POWER_OFF: case CART_SYNC_POWER_ON: return cart_power_sync(op);
    case CART_SYNC_XC_LOOKUP: case CART_SYNC_XC_RESERVE: case CART_SYNC_XC_COMMIT: return cart_ready() && xcache_sync(op, a, b);
    case CART_SYNC_PASS: case CART_SYNC_CACHE_COMMIT: return cart_ready() && cart_pass_sync(op, a, b);
    default: return false;
    }
}

static void __not_in_flash_func(n64_info)(uint32_t *w) {
    cart_pass_info(w);
    xcache_info(&w[CART_INFO(N64_MI_XC)], &w[CART_INFO(N64_MI_XC_BYTES)]);
    uint32_t id[2];
    cart_flashram_id(id);
    w[CART_INFO(N64_MI_SAVE_ID0)] = id[0], w[CART_INFO(N64_MI_SAVE_ID1)] = id[1];
}

// The cart's save: its EEPROM through cart_sram's RAM copy, or its FlashRAM passed through to the chip
static uint32_t __not_in_flash_func(n64_save_size)(void) { return cart_flashram_present() ? FRAM_BYTES : cart_sram_size(); }
static bool __not_in_flash_func(n64_save_read)(uint32_t off, uint32_t len, uint8_t *dst) {
    return cart_flashram_present() ? fram_read(off, len, dst) : cart_sram_read(off, len, dst);
}
static bool __not_in_flash_func(n64_save_write)(uint32_t off, uint32_t len, const uint8_t *src) {
    return cart_flashram_present() ? fram_write(off, len, src) : cart_eeprom_write(off, len, src);
}
static void __not_in_flash_func(n64_save_status)(uint32_t *pending, uint32_t *given_up) {
    if (cart_flashram_present()) fram_status(pending, given_up);
    else cart_sram_status(pending, given_up);
}

static const xcache_cfg_t g_xcache = { PSRAM_XC_INDEX, PSRAM_XC_STAGE, cart_header_title };

// What the Maple cart unit serves, all called on core1: ROM, CRC, save, pass tables, scan cache, xcache, sync, info.
static const maple_cart_t g_cart = {
    .rom_span    = 64u << 20,       // cart domain 1 address 2; past it is other cart address space
    .read        = cart_stream_read,
    .crc         = crc_dma,
    .save_size   = n64_save_size,
    .save_read   = n64_save_read,
    .save_write  = n64_save_write,
    .save_status = n64_save_status,
    .rom_size    = cart_rom_size,
    .tables_size = cart_tables_size,
    .tables_read = cart_tables_read,
    .cache_size  = cart_cache_size,
    .cache_read  = cart_cache_read,
    .cache_write = cart_cache_write,
    .xcache_size = xcache_size,
    .xcache_read = xcache_read,
    .xcache_write = xcache_write,
    .sync        = n64_sync,
    .info        = n64_info,
    .ready       = cart_ready,
};

int main(void) {
    MARK(1);
    boot_init();
    MARK(2);

    cart_power_on();    // before n64_bus_init, so no cart line is driven into an unpowered cart
    n64_bus_init();
    cic_init();
    cart_bringup(true);
    MARK(3);

    crc_dma_init();
    bool maple_up = dc_link_start(&g_cart, &g_cart_id);
    MARK(9);
    boot_done();
    uint32_t psram = psram_init();
    cart_attach();  // after boot_done: it reads flash, outside the boot watchdog's window
    sd_wait_hook = cart_stream_service;  // an SD write's card busy would otherwise leave core1 no ROM to serve
    xcache_init(&g_xcache);
    cart_power_ready();

    console_add(n64_commands, n64_ncommands);
    console_add(maple_commands, maple_ncommands);
    console_add(sd_commands, sd_ncommands);
    console_add(psram_commands, psram_ncommands);
    console_add(xcache_commands, xcache_ncommands);
    printf("\nn64link ready, psram %lu KB, ? for help%s\n", (unsigned long)(psram >> 10), maple_up ? "" : "  (maple failed to start)");
    for (;;) {
        cart_stream_service();
        cart_sram_service();
        cart_flashram_service();
        cart_pass_service();
        xcache_service();
        cart_power_service();
        console_poll();
    }
}
