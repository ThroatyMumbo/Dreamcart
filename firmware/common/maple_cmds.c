#include <stdio.h>
#include <stdlib.h>
#include "hardware/clocks.h"
#include "cart_stream.h"
#include "maple/dc_link.h"
#include "maple_cmds.h"

static void print_tx_timing(void) {
    unsigned h, s, m;
    mapledev_tx_timing(MAPLEDEV_TX_FAST, &h, &s, &m);
    unsigned cycles = 6 + 2 * (h + s + m);
    uint32_t ns_byte = (uint32_t)(4000000000ull * cycles / mapledev_tx_hz(MAPLEDEV_TX_FAST));
    printf("cart reply timing hold %u settle %u mid %u: %u cycles a bit pair, %lu ns a byte\n",
           h, s, m, cycles, (unsigned long)ns_byte);
}

static void cmd_maple(const char *arg) {
    if (!dc_link_running()) {
        printf("maple  not running: pio2 or a DMA channel could not be claimed\n");
    } else {
        const mapledev_stats_t *s = dc_link_stats();
        printf("maple  frames %lu replies %lu  bad_ev %lu bad_frame %lu desync %lu  overruns %lu no_idle %lu\n",
               (unsigned long)s->frames, (unsigned long)s->replies, (unsigned long)s->bad_ev,
               (unsigned long)s->bad_frame, (unsigned long)s->desync, (unsigned long)s->overruns,
               (unsigned long)s->no_idle);
        printf("       resyncs %lu frame_timeout %lu rx_stall %lu/%lu  turnaround %lu/%lu/%lu us  tx_max %lu us (%lu B)\n",
               (unsigned long)s->resyncs, (unsigned long)s->frame_timeout, (unsigned long)s->rx_stall,
               (unsigned long)s->rx_stall_drain, (unsigned long)s->turn_min, (unsigned long)s->turn_last,
               (unsigned long)s->turn_max, (unsigned long)s->tx_max, (unsigned long)s->tx_bytes_max);
        printf("       reply bit clock: controller %lu kHz, cart %lu kHz\n",
               (unsigned long)(mapledev_tx_hz(MAPLEDEV_TX_NOMINAL) / 1000),
               (unsigned long)(mapledev_tx_hz(MAPLEDEV_TX_FAST) / 1000));
        printf("       ");
        print_tx_timing();
        const cart_stream_stats_t *c = cart_stream_stats();
        printf("cart   served %lu not_ready %lu seeks %lu prefetched %lu  read_max %lu us gap_max %lu us\n",
               (unsigned long)c->served, (unsigned long)c->not_ready, (unsigned long)c->seeks,
               (unsigned long)c->filled_total, (unsigned long)c->read_us_max, (unsigned long)c->gap_us_max);
    }
    if (*arg == '0') cart_stream_clear_max();
}

static void cmd_tx_hz(const char *arg) {
    if (*arg) {
        uint32_t khz = strtoul(arg, NULL, 0), sys_khz = clock_get_hz(clk_sys) / 1000u;
        mapledev_set_tx_hz(MAPLEDEV_TX_FAST, (khz < sys_khz ? khz : sys_khz) * 1000u);
    }
    printf("cart reply bit clock %lu kHz\n", (unsigned long)(mapledev_tx_hz(MAPLEDEV_TX_FAST) / 1000));
}

static void cmd_tx_timing(const char *arg) {
    char *end;
    unsigned h = (unsigned)strtoul(arg, &end, 0), s = (unsigned)strtoul(end, &end, 0);
    unsigned m = (unsigned)strtoul(end, NULL, 0);
    if (*arg && !mapledev_set_tx_timing(MAPLEDEV_TX_FAST, h, s, m))
        printf("refused: hold 1-15, settle 0-15, mid 1-15 (or the TX program changed)\n");
    print_tx_timing();
}

static void cmd_buttons(const char *arg) {
    char *end;
    uint16_t held = (uint16_t)strtoul(arg, &end, 16);
    unsigned trig_l = (unsigned)strtoul(end, &end, 0);
    unsigned trig_r = (unsigned)strtoul(end, NULL, 0);
    if (trig_l > 255) trig_l = 255;
    if (trig_r > 255) trig_r = 255;
    dc_link_set_buttons(held);
    dc_link_set_triggers((uint8_t)trig_l, (uint8_t)trig_r);
    printf("controller holding %04X, L %u R %u\n", held, trig_l, trig_r);
}

const console_cmd_t maple_commands[] = {
    { 'u', "u [0]      Maple bus counters (Dreamcast controller on GP32/GP33); 0 clears the cart maxima after",
      cmd_maple },
    { 'b', "b [khz]    cart unit's Maple reply bit clock, default 200000; the controller stays at 41667", cmd_tx_hz },
    { 't', "t [h s m]  cart unit's reply data-loop delays hold/settle/mid, default 3 6 6", cmd_tx_timing },
    { 'j', "j [hex [l r]] hold controller buttons: 4 A, 2 B, 400 X, 200 Y, 8 Start, 10/20/40/80 U/D/L/R,\n"
           "           plus L/R triggers 0-255; 0 releases", cmd_buttons },
};

const unsigned maple_ncommands = sizeof maple_commands / sizeof maple_commands[0];
