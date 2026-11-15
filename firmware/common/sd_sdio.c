// SD-bus backend; command and data transfer ported from carlk3's rp2040_sdio.c, bring-up after VHSGame's sd.c.
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "board_common.h"
#include "pio_share.h"
#include "sd_int.h"
#include "sd_sdio.pio.h"

#define MAX_BLOCKS   128u               // per CMD18; longer reads are split
#define RING_BLOCKS  (SD_SCRATCH_WORDS / 128u)
#define CMD_US       10000u
#define RX_US        250000u
#define BUSY_US      250000u
#define WRITE_US     500000u            // SDXC's write busy limit
#define INIT_US      1000000u
#define R1_ERRORS    0xFDFFE008u        // card status error bits, as STM32's SDMMC_OCR_ERRORBITS

static PIO const pio = PIO_SD;

static struct {
    bool claimed, ring, overrun;
    uint sm_cmd, sm_data, dma_data, dma_ctrl;
    uint off_cmd, off_rx, off_tx;
    uint32_t clkdiv, rca;
    pio_sm_config cfg_rx, cfg_tx;
    uint32_t tail[3];                           // a written block's CRC words and end bit
    uint32_t *buf, words, total, done, checked, crc_bad;
    struct { void *addr; uint32_t count; } ctrl[2 * MAX_BLOCKS + 1];   // the control channel's reload list
    struct { uint32_t top, bottom; } crc[MAX_BLOCKS];
} s;

// The four lines' CRC-16s at once, 16 bits each, as carlk3's sdio_crc16_4bit_checksum.
__attribute__((optimize("Ofast")))
static uint64_t crc16_4bit(const uint32_t *data, uint32_t words) {
    uint64_t crc = 0;
    for (const uint32_t *end = data + words; data < end;) {
        uint32_t in = __builtin_bswap32(*data++);
        uint32_t out = (uint32_t)(crc >> 32);
        crc <<= 32;
        out ^= out >> 16;
        out ^= in >> 16;
        uint64_t x = out ^ in;
        crc ^= x ^ x << 20 ^ x << 48;
    }
    return crc;
}

static uint32_t sync_mask(void) { return 0x3Fu << (PIN_SD_CLK - pio_get_gpio_base(pio)); }   // CLK..D3

static void sdio_load(void) {
    s.off_cmd = pio_add_program(pio, &sdio_cmd_clk_program);
    s.off_rx = pio_add_program(pio, &sdio_data_rx_program);
    s.off_tx = pio_add_program(pio, &sdio_data_tx_program);

    pio_sm_config c = sdio_cmd_clk_program_get_default_config(s.off_cmd);
    sm_config_set_out_pins(&c, PIN_SD_CMD, 1);
    sm_config_set_in_pins(&c, PIN_SD_CMD);
    sm_config_set_set_pins(&c, PIN_SD_CMD, 1);
    sm_config_set_jmp_pin(&c, PIN_SD_CMD);
    sm_config_set_sideset_pins(&c, PIN_SD_CLK);
    sm_config_set_out_shift(&c, false, true, 32);
    sm_config_set_in_shift(&c, false, true, 32);
    sm_config_set_clkdiv_int_frac8(&c, s.clkdiv, 0);
    sm_config_set_mov_status(&c, STATUS_TX_LESSTHAN, 2);
    pio_sm_init(pio, s.sm_cmd, s.off_cmd, &c);
    pio_sm_set_consecutive_pindirs(pio, s.sm_cmd, PIN_SD_CLK, 1, true);
    pio_sm_set_enabled(pio, s.sm_cmd, true);

    s.cfg_rx = sdio_data_rx_program_get_default_config(s.off_rx);
    sm_config_set_in_pins(&s.cfg_rx, PIN_SD_D0);
    sm_config_set_in_shift(&s.cfg_rx, false, true, 32);
    sm_config_set_out_shift(&s.cfg_rx, false, true, 32);
    sm_config_set_clkdiv_int_frac8(&s.cfg_rx, s.clkdiv, 0);

    s.cfg_tx = sdio_data_tx_program_get_default_config(s.off_tx);
    sm_config_set_in_pins(&s.cfg_tx, PIN_SD_D0);
    sm_config_set_set_pins(&s.cfg_tx, PIN_SD_D0, 4);
    sm_config_set_out_pins(&s.cfg_tx, PIN_SD_D0, 4);
    sm_config_set_in_shift(&s.cfg_tx, false, false, 32);
    sm_config_set_out_shift(&s.cfg_tx, false, true, 32);
    sm_config_set_clkdiv_int_frac8(&s.cfg_tx, s.clkdiv, 0);

    pio->input_sync_bypass |= sync_mask();      // CLK is ours and synchronous to clk_sys
}

static void rx_stop(void) {
    dma_channel_abort(s.dma_ctrl);
    dma_channel_abort(s.dma_data);
    dma_channel_abort(s.dma_ctrl);              // in case the data channel's abort chained into it
    pio_sm_set_enabled(pio, s.sm_data, false);
    pio_sm_set_consecutive_pindirs(pio, s.sm_data, PIN_SD_D0, 4, false);
}

static void sdio_unload(void) {
    rx_stop();
    pio_sm_set_enabled(pio, s.sm_cmd, false);
    pio->input_sync_bypass &= ~sync_mask();
    pio_remove_program(pio, &sdio_cmd_clk_program, s.off_cmd);
    pio_remove_program(pio, &sdio_data_rx_program, s.off_rx);
    pio_remove_program(pio, &sdio_data_tx_program, s.off_tx);
}

static const pio_tenant_t tenant = { sdio_load, sdio_unload };

static bool claim(void) {
    if (s.claimed) return true;
    int a = pio_claim_unused_sm(pio, false), b = pio_claim_unused_sm(pio, false);
    if (a < 0 || b < 0) {
        if (a >= 0) pio_sm_unclaim(pio, (uint)a);
        if (b >= 0) pio_sm_unclaim(pio, (uint)b);
        return false;
    }
    s.sm_cmd = (uint)a;
    s.sm_data = (uint)b;
    s.dma_data = (uint)dma_claim_unused_channel(true);
    s.dma_ctrl = (uint)dma_claim_unused_channel(true);
    return s.claimed = true;
}

static void pins_setup(void) {
    for (uint p = PIN_SD_CLK; p <= PIN_SD_D3; p++) {
        if (p == PIN_SD_CLK) gpio_disable_pulls(p);
        else gpio_pull_up(p);                   // D3 high before CMD0, or the card picks SPI
        gpio_set_slew_rate(p, GPIO_SLEW_RATE_FAST);
        pio_gpio_init(pio, p);
    }
    gpio_set_drive_strength(PIN_SD_CLK, GPIO_DRIVE_STRENGTH_8MA);
}

static uint32_t set_clock(uint32_t hz) {
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t div = (sys + CLKDIV * hz - 1) / (CLKDIV * hz);
    s.clkdiv = div ? div : 1;
    pio_sm_set_clkdiv_int_frac8(pio, s.sm_cmd, s.clkdiv, 0);
    sm_config_set_clkdiv_int_frac8(&s.cfg_rx, s.clkdiv, 0);
    sm_config_set_clkdiv_int_frac8(&s.cfg_tx, s.clkdiv, 0);
    pio_clkdiv_restart_sm_mask(pio, 1u << s.sm_cmd);
    return sys / (CLKDIV * s.clkdiv);
}

static void send(uint8_t cmd, uint32_t arg, uint32_t resp_bits) {
    sd_stats.stage = cmd;
    uint8_t f[5] = {(uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8),
                    (uint8_t)arg};
    uint32_t w0 = 47u << 24 | (uint32_t)f[0] << 16 | (uint32_t)f[1] << 8 | f[2];
    uint32_t w1 = (uint32_t)f[3] << 24 | (uint32_t)f[4] << 16 | (uint32_t)sd_crc7(f, 5) << 9 | 1u << 8 |
                  (resp_bits ? resp_bits - 1 : 0);
    pio_sm_clear_fifos(pio, s.sm_cmd);
    pio_sm_put(pio, s.sm_cmd, w0);
    pio_sm_put(pio, s.sm_cmd, w1);
}

static void cmd_reset(void) {
    pio_sm_clear_fifos(pio, s.sm_cmd);
    pio_sm_exec(pio, s.sm_cmd, pio_encode_jmp(s.off_cmd));
}

static bool wait_words(uint8_t cmd, uint n) {
    uint32_t t0 = time_us_32();
    while (pio_sm_get_rx_fifo_level(pio, s.sm_cmd) < n)
        if (time_us_32() - t0 > CMD_US) {
            cmd_reset();
            sd_fail(cmd, SD_FAIL_NO_RESPONSE);
            return false;
        }
    return true;
}

// 48-bit response: r0 = start, direction, index and content 31..8; r1's low 16 bits = content 7..0, CRC, end.
static bool cmd_r1(uint8_t cmd, uint32_t arg, uint32_t *resp) {
    send(cmd, arg, resp ? 48 : 0);
    if (!wait_words(cmd, resp ? 2 : 1)) return false;
    if (!resp) {
        pio_sm_get(pio, s.sm_cmd);
        return true;
    }
    uint32_t r0 = pio_sm_get(pio, s.sm_cmd), r1 = pio_sm_get(pio, s.sm_cmd);
    uint8_t b[5] = {(uint8_t)(r0 >> 24), (uint8_t)(r0 >> 16), (uint8_t)(r0 >> 8), (uint8_t)r0, (uint8_t)(r1 >> 8)};
    *resp = sd_stats.resp = (r0 & 0xFFFFFF) << 8 | (r1 >> 8 & 0xFF);
    if (sd_crc7(b, 5) != (r1 >> 1 & 0x7F) || (b[0] & 0x3F) != cmd) {
        sd_fail(cmd, SD_FAIL_RESPONSE);
        return false;
    }
    return true;
}

// R3 (OCR) carries no CRC and index 0x3F.
static bool cmd_r3(uint8_t cmd, uint32_t arg, uint32_t *resp) {
    send(cmd, arg, 48);
    if (!wait_words(cmd, 2)) return false;
    uint32_t r0 = pio_sm_get(pio, s.sm_cmd), r1 = pio_sm_get(pio, s.sm_cmd);
    *resp = sd_stats.resp = (r0 & 0xFFFFFF) << 8 | (r1 >> 8 & 0xFF);
    return true;
}

// 136-bit response (CID, CSD) by DMA: more than the FIFO holds.
static bool cmd_r2(uint8_t cmd, uint32_t arg, uint8_t *out) {
    uint32_t w[5];
    pio_sm_clear_fifos(pio, s.sm_cmd);
    dma_channel_config d = dma_channel_get_default_config(s.dma_data);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, true);
    channel_config_set_dreq(&d, pio_get_dreq(pio, s.sm_cmd, false));
    dma_channel_configure(s.dma_data, &d, w, &pio->rxf[s.sm_cmd], 5, true);
    send(cmd, arg, 136);
    uint32_t t0 = time_us_32();
    while (dma_channel_is_busy(s.dma_data))
        if (time_us_32() - t0 > CMD_US) {
            dma_channel_abort(s.dma_data);
            cmd_reset();
            sd_fail(cmd, SD_FAIL_NO_RESPONSE);
            return false;
        }
    for (int i = 0; i < 15; i++) {
        uint32_t bit = 8u * (unsigned)(i + 1);             // byte i sits after the start/direction/index byte
        out[i] = (uint8_t)(w[bit / 32] >> (24 - bit % 32));
    }
    out[15] = (uint8_t)w[4];
    if ((w[0] >> 24 & 0x3F) != 0x3F || sd_crc7(out, 15) != out[15] >> 1) {
        sd_fail(cmd, SD_FAIL_RESPONSE);
        return false;
    }
    return true;
}

static bool wait_idle(void) {
    uint32_t t0 = time_us_32();
    while (!gpio_get(PIN_SD_D0)) {
        if (time_us_32() - t0 > BUSY_US) {
            sd_fail(sd_stats.stage, SD_FAIL_NO_DATA);
            return false;
        }
        sd_wait_tick();
    }
    return true;
}

static uint32_t blocks_done(void) {
    uint32_t n = (dma_hw->ch[s.dma_ctrl].read_addr - (uint32_t)(uintptr_t)s.ctrl) / sizeof s.ctrl[0];
    return n ? (n - 1) / 2 : 0;
}

// ring: blocks cycle through RING_BLOCKS slots of buf and must be checked before they are overwritten.
static void rx_start(uint32_t *buf, uint32_t blocks, uint32_t bytes, bool ring) {
    s.buf = buf;
    s.words = bytes / 4;
    s.total = blocks;
    s.ring = ring;
    s.done = s.checked = s.crc_bad = 0;
    s.overrun = false;
    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t slot = ring ? i % RING_BLOCKS : i;
        s.ctrl[2 * i].addr = buf + slot * s.words;
        s.ctrl[2 * i].count = s.words;
        s.ctrl[2 * i + 1].addr = &s.crc[slot];
        s.ctrl[2 * i + 1].count = 2;
    }
    s.ctrl[2 * blocks].addr = NULL;             // a zero count is a null trigger: the chain ends
    s.ctrl[2 * blocks].count = 0;

    dma_channel_config d = dma_channel_get_default_config(s.dma_data);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, true);
    channel_config_set_dreq(&d, pio_get_dreq(pio, s.sm_data, false));
    channel_config_set_bswap(&d, true);
    channel_config_set_chain_to(&d, s.dma_ctrl);
    dma_channel_configure(s.dma_data, &d, NULL, &pio->rxf[s.sm_data], 0, false);

    d = dma_channel_get_default_config(s.dma_ctrl);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, true);
    channel_config_set_ring(&d, true, 3);       // write_addr then transfer_count_trig, every time
    dma_channel_configure(s.dma_ctrl, &d, &dma_hw->ch[s.dma_data].al1_write_addr, s.ctrl, 2, false);

    pio_sm_init(pio, s.sm_data, s.off_rx, &s.cfg_rx);
    pio_sm_set_consecutive_pindirs(pio, s.sm_data, PIN_SD_D0, 4, false);
    pio_sm_put(pio, s.sm_data, bytes * 2 + 16 - 1);
    pio_sm_exec(pio, s.sm_data, pio_encode_out(pio_y, 32));
    pio->sm[s.sm_data].shiftctrl |= PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS;
    dma_channel_start(s.dma_ctrl);
    pio_sm_set_enabled(pio, s.sm_data, true);
}

static void verify(uint32_t max) {
    while (s.checked < s.done && max--) {
        uint32_t b = s.checked++, slot = s.ring ? b % RING_BLOCKS : b;
        const uint32_t *data = s.buf + slot * s.words;
        uint64_t got = crc16_4bit(data, s.words);
        uint64_t want = (uint64_t)__builtin_bswap32(s.crc[slot].top) << 32 | __builtin_bswap32(s.crc[slot].bottom);
        if (s.ring) {
            sd_stats.sum += sd_word_sum(data, s.words);
            if (blocks_done() >= b + RING_BLOCKS) {
                s.overrun = true;
                continue;
            }
        }
        if (got != want) s.crc_bad++;
    }
}

static bool rx_wait(uint8_t cmd) {
    uint32_t t0 = time_us_32();
    while ((s.done = blocks_done()) < s.total) {
        verify(4);
        if (time_us_32() - t0 > RX_US) {
            rx_stop();
            sd_fail(cmd, SD_FAIL_NO_DATA);
            return false;
        }
    }
    rx_stop();
    verify(s.total);
    if (s.overrun) {
        sd_fail(cmd, SD_FAIL_OVERRUN);
        return false;
    }
    if (s.crc_bad) {
        sd_fail(cmd, SD_FAIL_DATA_CRC);
        sd_stats.crc_errors += s.crc_bad - 1;
        return false;
    }
    return true;
}

static bool cmd6(uint32_t arg) {
    uint32_t r;
    if (!wait_idle()) return false;
    rx_start(sd_scratch, 1, 64, false);
    if (!cmd_r1(6, arg, &r)) {
        rx_stop();
        return false;
    }
    return rx_wait(6);
}

static bool switch_high_speed(sd_card_t *c, bool want) {
    const uint8_t *st = (const uint8_t *)sd_scratch;
    if (!cmd6(0x00FFFFF1u)) return false;
    c->hs_support = sd_switch_support(st);
    if (!want || !(c->hs_support & 0x02)) return false;
    return cmd6(0x80FFFFF1u) && sd_switch_selected(st);
}

static bool init_once(sd_card_t *c, uint32_t hz) {
    uint32_t r;
    set_clock(400000);
    busy_wait_ms(1);                            // well over the 74 clocks a card wants before CMD0
    cmd_r1(0, 0, NULL);
    if (!cmd_r1(8, 0x1AA, &r)) return false;
    if ((r & 0xFFF) != 0x1AA) {
        sd_fail(8, SD_FAIL_RESPONSE);
        return false;
    }
    uint32_t t0 = time_us_32();
    do {
        if (!cmd_r1(55, 0, &r) || !cmd_r3(41, 0x40FF8000u, &c->ocr)) return false;
        if (time_us_32() - t0 > INIT_US) {
            sd_fail(41, SD_FAIL_RESPONSE);
            return false;
        }
        if (!(c->ocr & 0x80000000u)) busy_wait_ms(5);
    } while (!(c->ocr & 0x80000000u));
    c->block_addr = c->ocr & 0x40000000u;

    if (!cmd_r2(2, 0, c->cid) || !cmd_r1(3, 0, &r)) return false;
    s.rca = r & 0xFFFF0000u;
    if (!cmd_r2(9, s.rca, c->csd) || !cmd_r1(7, s.rca, &r) || !wait_idle()) return false;
    if (!cmd_r1(55, s.rca, &r) || !cmd_r1(42, 0, &r)) return false;     // drops DAT3's card-detect pull-up
    if (!cmd_r1(55, s.rca, &r) || !cmd_r1(6, 2, &r)) return false;      // ACMD6: 4-bit bus
    if (!cmd_r1(16, SD_SECTOR, &r)) return false;
    c->sectors = sd_csd_sectors(c->csd);
    c->hz = set_clock(hz > SD_DEFAULT_HZ ? SD_DEFAULT_HZ : hz);
    c->high_speed = switch_high_speed(c, hz > SD_DEFAULT_HZ);
    if (c->high_speed) c->hz = set_clock(hz > SD_MAX_HZ ? SD_MAX_HZ : hz);
    sd_stats.stage = 100;
    return true;
}

bool sd_sdio_init(sd_card_t *c, uint32_t hz) {
    if (!claim()) return false;
    pins_setup();
    pio_share_enter(pio, &tenant);
    for (int i = 0; i < 3; i++)
        if (init_once(c, hz)) return true;
    return false;
}

void sd_sdio_release(void) {
    if (s.claimed) pio_share_leave(pio, &tenant);
}

// One 512 B block out on D0-D3: start nibble, data, the four CRC-16s, end bit; then the CRC status token and busy.
static bool tx_block(const uint32_t *data) {
    uint64_t crc = crc16_4bit(data, 128);
    s.tail[0] = (uint32_t)(crc >> 32), s.tail[1] = (uint32_t)crc, s.tail[2] = 0xFFFFFFFFu;

    pio_sm_init(pio, s.sm_data, s.off_tx, &s.cfg_tx);
    pio_sm_exec(pio, s.sm_data, pio_encode_set(pio_pins, 0xF));
    pio_sm_set_consecutive_pindirs(pio, s.sm_data, PIN_SD_D0, 4, true);

    dma_channel_config d = dma_channel_get_default_config(s.dma_data);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, true);
    channel_config_set_write_increment(&d, false);
    channel_config_set_dreq(&d, pio_get_dreq(pio, s.sm_data, true));
    channel_config_set_bswap(&d, true);
    channel_config_set_chain_to(&d, s.dma_ctrl);
    dma_channel_configure(s.dma_data, &d, &pio->txf[s.sm_data], data, 128, false);
    channel_config_set_bswap(&d, false);
    channel_config_set_chain_to(&d, s.dma_ctrl);    // chaining to itself: none
    dma_channel_configure(s.dma_ctrl, &d, &pio->txf[s.sm_data], s.tail, 3, false);

    pio_sm_put(pio, s.sm_data, 8 + 1024 + 16 + 1 - 1);     // nibbles: start word, data, CRC, end
    pio_sm_exec(pio, s.sm_data, pio_encode_out(pio_x, 32));
    pio_sm_put(pio, s.sm_data, 32 - 1);                     // D0 bits sampled for the status token
    pio_sm_exec(pio, s.sm_data, pio_encode_out(pio_y, 32));
    pio_sm_put(pio, s.sm_data, 0xFFFFFFF0u);
    dma_channel_start(s.dma_data);
    pio_sm_set_enabled(pio, s.sm_data, true);

    uint32_t t0 = time_us_32();
    while (pio_sm_is_rx_fifo_empty(pio, s.sm_data)) {
        if (time_us_32() - t0 > WRITE_US) {
            rx_stop();
            sd_fail(sd_stats.stage, SD_FAIL_NO_DATA);
            return false;
        }
        sd_wait_tick();
    }
    uint32_t r = pio_sm_get(pio, s.sm_data);
    rx_stop();
    sd_stats.resp = r;
    uint32_t ones = r == 0xFFFFFFFFu ? 32 : (uint32_t)__builtin_clz(~r);
    if (ones > 28 || (r << (ones + 1)) >> 29 != 2) {        // 010: accepted; 101 CRC error, 110 write error
        sd_fail(sd_stats.stage, SD_FAIL_WRITE);
        return false;
    }
    return true;
}

bool sd_sdio_write(const sd_card_t *c, uint32_t sector, uint32_t blocks, const uint8_t *src) {
    pio_share_enter(pio, &tenant);
    uint32_t r = 0;
    uint8_t cmd = blocks == 1 ? 24 : 25;
    if (!wait_idle()) return false;
    if (!cmd_r1(cmd, c->block_addr ? sector : sector * SD_SECTOR, &r) || (r & R1_ERRORS)) {
        if (r & R1_ERRORS) sd_fail(cmd, SD_FAIL_RESPONSE);
        return false;
    }
    bool ok = true;
    for (uint32_t k = 0; ok && k < blocks; k++) ok = tx_block((const uint32_t *)(src + SD_SECTOR * k));
    if (blocks > 1) ok = cmd_r1(12, 0, &r) && ok;
    uint32_t t0 = time_us_32();
    while (!gpio_get(PIN_SD_D0)) {
        if (time_us_32() - t0 > WRITE_US) {
            sd_fail(cmd, SD_FAIL_NO_DATA);
            return false;
        }
        sd_wait_tick();
    }
    return ok;
}

bool sd_sdio_read(const sd_card_t *c, uint32_t sector, uint32_t blocks, uint8_t *dst) {
    pio_share_enter(pio, &tenant);
    while (blocks) {
        uint32_t n = blocks < MAX_BLOCKS ? blocks : MAX_BLOCKS, r = 0;
        uint8_t cmd = n == 1 ? 17 : 18;
        if (!wait_idle()) return false;
        rx_start(dst ? (uint32_t *)dst : sd_scratch, n, SD_SECTOR, !dst);
        if (!cmd_r1(cmd, c->block_addr ? sector : sector * SD_SECTOR, &r) || (r & R1_ERRORS)) {
            if (r & R1_ERRORS) sd_fail(cmd, SD_FAIL_RESPONSE);
            rx_stop();
            return false;
        }
        bool ok = rx_wait(cmd);
        if (n > 1) {
            ok = cmd_r1(12, 0, &r) && ok;
            ok = wait_idle() && ok;
        }
        if (!ok) return false;
        sector += n;
        blocks -= n;
        if (dst) dst += SD_SECTOR * n;
    }
    return true;
}
