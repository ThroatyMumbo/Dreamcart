#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "board.h"
#include "pads.h"
#include "n64_bus.h"
#include "cart_info.h"
#include "cart_diag.h"
#include "cart_eeprom.h"
#include "cart_sram.h"
#include "cart_pass.h"
#include "cart_stream.h"
#include "cart_power.h"
#include "cart_sync.h"
#include "console.h"
#include "xcache.h"

#define RAIL_MS    50u      // U1 charging C4 + C8; TP3 is not on a GPIO
#define CIC_MS     300u     // cart clock running this long before reset lifts
#define RESET_MS   50u
#define QUIESCE_MS 20u      // a Maple command already past the ready check finishes well inside this
#define NPINS      (PIN_CART_LAST + 1)
_Static_assert(CART_STREAM_BLOCK == N64_BUS_BURST, "the ROM stream reads whole bus bursts");

static volatile bool g_ready = true, g_off;
static volatile uint8_t g_req;  // a BLOCK_SYNC power op core1 took, until core0 has carried it out
static struct { gpio_function_t fn; bool up, down; } g_pins[NPINS];

void cart_power_on(void) {
    pad_drive(PIN_CART_PWR_EN, 1);
    pad_drive(PIN_LED, 1);
    sleep_ms(RAIL_MS);
}

void cart_bringup(bool first) {
    sleep_ms(CIC_MS);
    n64_bus_set_reset(true);
    sleep_ms(RESET_MS);
    if (first) cart_eeprom_init();
    else cart_eeprom_reload();
    cart_stream_init(n64_bus_read, CART_ROM);
    cart_set_rom_mb(cart_diag_probe_size() >> 20);  // the console scans the whole ROM, so it needs the real size
}

void cart_attach(void) {
    cart_pass_init();
    uint8_t h[CART_HDR_LEN];
    cart_header_read(h);
    xcache_set_cart(h, cart_rom_size());
}

bool __not_in_flash_func(cart_ready)(void) { return g_ready; }

// PIO, PWM and SIO keep their state behind GPIO_FUNC_NULL, so restoring the function resumes each line as it was
static void pins_release(void) {
    for (uint p = 0; p < NPINS; p++) {
        g_pins[p].fn = gpio_get_function(p);
        g_pins[p].up = gpio_is_pulled_up(p);
        g_pins[p].down = gpio_is_pulled_down(p);
        gpio_set_function(p, GPIO_FUNC_NULL);
        gpio_disable_pulls(p);
    }
}

static void pins_restore(void) {
    for (uint p = 0; p < NPINS; p++) {
        gpio_set_pulls(p, g_pins[p].up, g_pins[p].down);
        gpio_set_function(p, g_pins[p].fn);
    }
}

static void report(void) {
    uint8_t h[CART_HDR_LEN];
    cart_header_read(h);
    uint32_t magic = cart_header_magic(h);
    if (magic != N64_MAGIC) {
        printf("no N64 header (magic %08lX): slot empty or cart not seated\n", (unsigned long)magic);
        return;
    }
    char title[21];
    cart_header_title(h, title);
    printf("cart \"%s\", %lu MB, eeprom %lu bytes, scan cache %s\n", title, (unsigned long)cart_rom_mb(),
           (unsigned long)cart_sram_size(), cart_cache_size() ? "loaded" : "none");
}

static void power_down(void) {
    if (g_off) return;
    g_ready = false;
    sleep_ms(QUIESCE_MS);
    if (!cart_sram_flush(2000)) printf("eeprom: writes still pending after 2 s, dropped\n");
    cart_pass_stop();
    n64_bus_set_reset(false);
    sleep_ms(1);
    pins_release();
    gpio_put(PIN_CART_PWR_EN, 0);
    gpio_put(PIN_LED, 0);
    g_off = true;
    printf("cart power off\n");
}

static void power_up(void) {
    if (!g_off) return;
    cart_power_on();
    pins_restore();
    cart_bringup(false);
    cart_attach();
    g_off = false;
    __dmb();
    g_ready = true;
    report();
}

void cart_swap(void) {
    power_down();
    printf("swap carts, then press Enter\n");
    console_wait_enter();
    power_up();
}

bool __not_in_flash_func(cart_power_sync)(uint32_t op) {
    if (g_req) return false;
    if (op == CART_SYNC_POWER_OFF ? !g_off : g_off) g_req = (uint8_t)op;
    return true;
}

void cart_power_service(void) {
    uint8_t op = g_req;
    if (!op) return;
    if (op == CART_SYNC_POWER_OFF) power_down();
    else power_up();
    __dmb();
    g_req = 0;
}
