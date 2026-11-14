#include "dc_link.h"
#include "pico/multicore.h"
#include "hardware/sync.h"
#include "board_common.h"

#define DC_IDLE_US 1000
// Cart replies only; the controller stays nominal. 200 MHz (divider 1) with 3/6/6 measured lossless,
// one step inside each console edge: settle >= 5, mid >= 4, hold + mid >= 8 (docs/maple_speed.md).
#define DC_CART_TX_HZ 200000000u
#define DC_CART_HOLD   3u
#define DC_CART_SETTLE 6u
#define DC_CART_MID    6u

static maple_bus_t  g_bus;
static maple_unit_t g_unit, g_cart;
static maple_cond_t g_cond;
static bool g_running;

// Enumeration replies are packed once, so a DEVICE_REQUEST costs only a port patch in the turnaround.
static mapledev_pkt_t g_info[MAPLE_MAX_UNITS], g_allinfo[MAPLE_MAX_UNITS], g_reply;

// Static, not stack: core1's stack is 2 KB.
static uint8_t g_req[MAPLE_MAX_FRAME], g_frame[MAPLE_MAX_FRAME];

// One pair per unit, indexed like g_bus, so each address answers with its own identity.
static void prebuild(void) {
    for (unsigned i = 0; i < g_bus.n; i++) {
        uint8_t req[4] = { MAPLE_CMD_DEVICE_REQUEST, g_bus.unit[i]->addr, MAPLE_ADDR_DC, 0 };
        mapledev_stage(&g_info[i], g_frame, maple_build_reply(&g_bus, req, 4, g_frame));
        req[0] = MAPLE_CMD_ALL_STATUS_REQUEST;
        mapledev_stage(&g_allinfo[i], g_frame, maple_build_reply(&g_bus, req, 4, g_frame));
    }
}

static void __not_in_flash_func(core1_main)(void) {
    save_and_disable_interrupts();
    for (;;) {
        unsigned rn;
        if (!mapledev_poll(g_req, &rn, DC_IDLE_US)) { continue; }

        // Also drops our own replies, which are addressed to the console.
        int ui = maple_bus_find(&g_bus, maple_dst(g_req));
        if (ui < 0) { mapledev_resync(); continue; }

        mapledev_pkt_t *pkt = NULL;
        switch (maple_cmd(g_req)) {
        case MAPLE_CMD_DEVICE_REQUEST:     pkt = &g_info[ui];    break;
        case MAPLE_CMD_ALL_STATUS_REQUEST: pkt = &g_allinfo[ui]; break;
        default: {
            unsigned n = maple_build_reply(&g_bus, g_req, rn, g_frame);
            if (n) { mapledev_stage(&g_reply, g_frame, n); pkt = &g_reply; }
            break;
        }
        }

        if (pkt) {
            mapledev_tx_select(g_bus.unit[ui] == &g_cart ? MAPLEDEV_TX_FAST : MAPLEDEV_TX_NOMINAL);
            mapledev_set_port(pkt, maple_src(g_req) & MAPLE_PORT_MASK);
            if (mapledev_send(pkt)) { mapledev_wait_sent(pkt); }
        }
        mapledev_resync();      // unconditional: the receiver watched our own reply go out
    }
}

bool dc_link_start(const maple_cart_t *cart, const maple_devid_t *id) {
    maple_cond_default(&g_cond);
    maple_bus_init(&g_bus);
    maple_controller_unit(&g_unit, &g_cond);
    maple_bus_add(&g_bus, &g_unit);
    maple_cart_unit(&g_cart, cart, id, MAPLE_SUB_UNIT(2));
    maple_bus_add(&g_bus, &g_cart);

    pio_set_gpio_base(PIO_MAPLE, 16);
    if (!mapledev_init(PIO_MAPLE, PIN_MAPLE_SDCKA, PIN_MAPLE_SDCKB)) { return false; }
    mapledev_set_tx_hz(MAPLEDEV_TX_FAST, DC_CART_TX_HZ);
    mapledev_set_tx_timing(MAPLEDEV_TX_FAST, DC_CART_HOLD, DC_CART_SETTLE, DC_CART_MID);
    prebuild();
    multicore_reset_core1();
    multicore_launch_core1(core1_main);
    g_running = true;
    return true;
}

bool dc_link_running(void) { return g_running; }

void dc_link_set_buttons(uint16_t pressed) {
    g_cond.buttons = (uint16_t)~pressed;
}

void dc_link_set_triggers(uint8_t left, uint8_t right) {
    g_cond.trig_l = left;
    g_cond.trig_r = right;
}

const mapledev_stats_t *dc_link_stats(void) { return mapledev_stats(); }
