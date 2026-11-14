#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "board.h"
#include "n64_bus.h"
#include "cic.h"
#include "cic_read.pio.h"
#include "pio_share.h"

// seeds and checksums from jago85/UltraCIC_C
static const struct { uint8_t seed; const char *name; uint8_t sum[12]; } CIC_ID[] = {
    { 0x3F, "CIC-NUS-6102 / 7101", { 0xA, 0x5, 0x3, 0x6, 0xC, 0x0, 0xF, 0x1, 0xD, 0x8, 0x5, 0x9 } },
    { 0x3F, "CIC-NUS-6101",        { 0x4, 0x5, 0xC, 0xC, 0x7, 0x3, 0xE, 0xE, 0x3, 0x1, 0x7, 0xA } },
    { 0x3F, "CIC-NUS-7102",        { 0x4, 0x4, 0x1, 0x6, 0x0, 0xE, 0xC, 0x5, 0xD, 0x9, 0xA, 0xF } },
    { 0x78, "CIC-NUS-6103 / 7103", { 0x5, 0x8, 0x6, 0xF, 0xD, 0x4, 0x7, 0x0, 0x9, 0x8, 0x6, 0x7 } },
    { 0x91, "CIC-NUS-6105 / 7105", { 0x8, 0x6, 0x1, 0x8, 0xA, 0x4, 0x5, 0xB, 0xC, 0x2, 0xD, 0x3 } },
    { 0x85, "CIC-NUS-6106 / 7106", { 0x2, 0xB, 0xB, 0xA, 0xD, 0x4, 0xE, 0x6, 0xE, 0xB, 0x7, 0x4 } },
};

static uint32_t g_cic_us = 100;

#define CIC_BITS 256
static uint8_t g_bits[CIC_BITS];

#define PIF_CLK_HZ 1966200u  // what the PIF drives on cart pin 19

uint32_t cic_half_us(void) { return g_cic_us; }

bool cic_set_half_us(uint32_t us) {
    if (us < 1 || us > CIC_HALF_US_MAX) return false;
    g_cic_us = us;
    return true;
}

static void cic_clock_init(void) {
    gpio_set_function(PIN_CIC_CLK, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_CIC_CLK);
    uint32_t div = (clock_get_hz(clk_sys) + PIF_CLK_HZ / 2) / PIF_CLK_HZ;  // 200 MHz / 102 = 1.961 MHz
    pwm_set_wrap(slice, (uint16_t)(div - 1));
    pwm_set_gpio_level(PIN_CIC_CLK, (uint16_t)(div / 2));
    pwm_set_enabled(slice, true);
}

// latch high before the driver is enabled; gpio_init leaves the latch at 0, and a CIC
// waiting on DCLK cannot tell that low glitch from the first clock edge
static void cic_lines_init(void) {
    gpio_put(PIN_CIC_DCLK, 1);
    gpio_set_dir(PIN_CIC_DCLK, GPIO_OUT);
    gpio_set_function(PIN_CIC_DCLK, GPIO_FUNC_SIO);

    gpio_set_function(PIN_CIC_DIO, GPIO_FUNC_SIO);
    gpio_set_dir(PIN_CIC_DIO, GPIO_IN);
    gpio_pull_up(PIN_CIC_DIO);  // CIC drives low for 0 and tristates for 1
}

void cic_init(void) {
    cic_clock_init();
    cic_lines_init();
}

static PIO cic_pio = PIO_JOYBUS;

// the handshake replays only after the CIC is re-armed, so every capture resets it first
static void cic_capture(uint8_t *bits, uint32_t half_us) {
    // loaded per capture into an emptied pio1; the EEPROM link or SDIO reloads on its next use
    pio_share_enter(cic_pio, NULL);
    uint cic_offset = pio_add_program(cic_pio, &cic_read_program);
    uint cic_sm = pio_claim_unused_sm(cic_pio, true);
    cic_lines_init();
    cic_read_program_init(cic_pio, cic_sm, cic_offset, PIN_CIC_DCLK, PIN_CIC_DIO, half_us);
    busy_wait_us_32(200);

    n64_bus_set_reset(false);
    sleep_ms(20);
    n64_bus_set_reset(true);
    sleep_ms(10);

    uint32_t words[CIC_BITS / 32];
    pio_sm_set_enabled(cic_pio, cic_sm, true);
    pio_sm_put_blocking(cic_pio, cic_sm, CIC_BITS - 1);
    for (int i = 0; i < CIC_BITS / 32; i++) words[i] = pio_sm_get_blocking(cic_pio, cic_sm);
    pio_sm_set_enabled(cic_pio, cic_sm, false);
    pio_sm_unclaim(cic_pio, cic_sm);
    pio_remove_program(cic_pio, &cic_read_program, cic_offset);
    for (int i = 0; i < CIC_BITS; i++) bits[i] = (uint8_t)((words[i / 32] >> (31 - i % 32)) & 1u);
}

// a PWM pin still reads back at the pad, so a dead or shorted CIC clock shows up here
static bool cic_clock_alive(void) {
    unsigned seen = 0;
    for (int i = 0; i < 256; i++) {
        seen |= gpio_get(PIN_CIC_CLK) ? 2u : 1u;
        busy_wait_us_32(1);
    }
    return seen == 3;
}

// inverse of UltraCIC's EncodeRound; element 0 is left untouched by the forward pass
static void decode_round(uint8_t *m, int n) {
    uint8_t a = m[0];
    for (int i = 1; i < n; i++) {
        uint8_t t = (uint8_t)((m[i] - a - 1) & 0x0F);
        a = m[i];
        m[i] = t;
    }
}

static uint8_t nib_at(const uint8_t *b, int off) {
    return (uint8_t)((b[off] << 3) | (b[off + 1] << 2) | (b[off + 2] << 1) | b[off + 3]);
}

typedef struct {
    bool any_low, any_high, framed;
    int off;
    uint8_t hello, seed, sum[12];
    const char *name;
} cic_res_t;

// 4 bits hello, 24 seed, 1 separator, 64 checksum; UltraCIC_C main()
#define CIC_FRAME 93

static bool cic_decode(const uint8_t *bits, cic_res_t *r) {
    memset(r, 0, sizeof *r);
    for (int i = 0; i < CIC_BITS; i++) {
        if (bits[i]) r->any_high = true; else r->any_low = true;
    }
    if (!r->any_low) return false;

    // the wire seed always decodes to B 5 h l h l, so search every bit offset for that frame
    for (int off = 4; off + CIC_FRAME - 4 <= CIC_BITS; off++) {
        uint8_t s[6];
        for (int i = 0; i < 6; i++) s[i] = nib_at(bits, off + 4 * i);
        decode_round(s, 6);
        decode_round(s, 6);
        if (!(s[0] == 0xB && s[1] == 0x5 && s[2] == s[4] && s[3] == s[5])) continue;

        uint8_t c[16];
        for (int i = 0; i < 16; i++) c[i] = nib_at(bits, off + 25 + 4 * i);
        for (int i = 0; i < 4; i++) decode_round(c, 16);

        r->framed = true;
        r->off = off;
        r->hello = nib_at(bits, off - 4);
        r->seed = (uint8_t)((s[2] << 4) | s[3]);
        memcpy(r->sum, c + 4, 12);

        for (unsigned j = 0; j < sizeof CIC_ID / sizeof CIC_ID[0]; j++) {
            if (CIC_ID[j].seed == r->seed && memcmp(CIC_ID[j].sum, r->sum, 12) == 0) {
                r->name = CIC_ID[j].name;
                break;
            }
        }
        return true;
    }
    return false;
}

static void cic_report_raw(const uint8_t *bits) {
    printf("raw    ");
    for (int i = 0; i + 4 <= CIC_BITS; i += 4) printf("%X", nib_at(bits, i));
    printf("\n");
}

static void cic_report(const cic_res_t *r, uint32_t half_us) {
    if (!r->any_low) {
        printf("CIC    no response, DIO stayed high for all %d bits\n", CIC_BITS);
        printf("       dead CIC, no cart power, or GP%d/GP%d/GP%d not on cart pins 18/43/19\n",
               PIN_CIC_DIO, PIN_CIC_DCLK, PIN_CIC_CLK);
        return;
    }
    if (!r->any_high) {
        printf("CIC    DIO stayed low; shorted to GND, or the pull-up is not reaching it\n");
        return;
    }
    if (!r->framed) {
        printf("CIC    DIO toggles but no seed frame at any bit offset\n");
        printf("       CIC is responding out of spec, or DCLK and DIO are swapped\n");
        return;
    }
    printf("frame  seed at bit %d, DCLK half period %lu us\n", r->off, (unsigned long)half_us);
    printf("hello  %X  %s\n", r->hello,
           r->hello == 0x1 ? "NTSC" : r->hello == 0x5 ? "PAL" : "unexpected");
    printf("seed   %02X\n", r->seed);
    printf("chksum ");
    for (int i = 0; i < 12; i++) printf("%X", r->sum[i]);
    printf("\n");
    if (r->name) printf("match  %s -- CIC alive and correct\n", r->name);
    else         printf("match  frame decodes but this seed/checksum is not in the table\n");
}

void cic_cmd(void) {
    const uint32_t tries[] = { g_cic_us, 30, 300 };
    cic_res_t r;
    uint32_t used = g_cic_us;
    bool ok = false;

    if (!cic_clock_alive())
        printf("warn   GP%d not toggling, the CIC has no clock (cart pin 19)\n", PIN_CIC_CLK);

    for (unsigned i = 0; i < sizeof tries / sizeof tries[0] && !ok; i++) {
        cic_capture(g_bits, tries[i]);
        used = tries[i];
        ok = cic_decode(g_bits, &r);
    }

    cic_report_raw(g_bits);
    cic_report(&r, used);
}
