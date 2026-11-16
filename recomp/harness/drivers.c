// The boot driver: a cart's power-on boot under the OS and RCP HLE; dc/play.c owns the RCP hooks, the pad and the save
#include "drivers.h"
#include "sim.h"
#include "calls.h"
#include "os.h"
#include "rcp.h"
#include "pif.h"
#include "cartinfo.h"

static void boot_die(void) {
    const uint8_t *e = pif_eeprom();
    for (int blk = 0; blk < (int)(pif_eeprom_bytes() / 8); blk++) {
        const uint8_t *b = e + 8 * blk;
        sim_hex("eep", (uint32_t)blk);
        sim_hex("", (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]);
        sim_hex("", (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7]);
        sim_puts("\n");
    }
    sim_hex("ret", 0x95);
    sim_hex("mem", os_state_hash());
    sim_hex("ops", 0);
    sim_hex("arg", os_switches);
    sim_puts("\n");
}

void os_driver_main(void) {
    call_init();
    os_init(cart_info.sp);
    rcp_boot_spmem(cart_info.sp_fill, cart_info.imem0);
    os_hooks.die = boot_die;
    const uint32_t a[8] = { 0 };
    os_call(cart_info.main, a);
    os_die("os_boot: main returned", 0);
}
