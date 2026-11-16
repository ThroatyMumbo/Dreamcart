#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "board.h"
#include "n64_bus.h"
#include "console.h"
#include "cart_sync.h"
#include "cart_flashram.h"

#define BLOCK_BYTES N64_BUS_BURST
#define STAGE       2048u       // core1 serves reads from here; two of the console's 1008-byte CRC reads
#define POLL_US     500u
#define CYCLE_MS    200u        // erase plus program, from the datasheets' worst case with margin
#define TRIES       3u

#define CMD_ERASE_PAGE 0x4B000000u
#define CMD_ERASE_RUN  0x78000000u
#define CMD_PROGRAM    0xA5000000u
#define CMD_BUFFER     0xB4000000u
#define CMD_STATUS     0xD2000000u
#define CMD_ID         0xE1000000u
#define CMD_READ       0xF0000000u
#define ST_ERASE_BUSY  0x02u
#define ST_WRITE_BUSY  0x01u

// id byte 7 from sanni's getFramType; the 1100 maps each 128-byte page onto 64 bytes of address
static const struct { uint8_t code; const char *name; bool halved; } FRAM_ID[] = {
    { 0x1E, "MX29L1100",                    true  },
    { 0x1D, "MX29L1101",                    false },
    { 0xF1, "MN63F81MPN",                   false },
    { 0x8E, "29L1100KC-15B0 (as MX29L1101)", false },
    { 0x84, "29L1100KC-15B0 (as MX29L1101)", false },
};

static int g_type = -1;                 // core0 writes it at attach, before the ready flag lets core1 in
static uint32_t g_id[2], g_mode;

// Core1 -> core0
static volatile uint32_t g_want, g_want_seq, g_want_ack;
static volatile bool g_wbusy;
static uint8_t g_page[FRAM_PAGE];
static uint32_t g_page_off;
// Core0 -> core1: [g_st_off, +g_st_len) of the chip, while g_st_gen is even
static uint8_t g_stage[STAGE];
static volatile uint32_t g_st_off, g_st_len, g_st_gen;
static volatile uint32_t g_given_up;

enum { W_IDLE, W_ERASE, W_PROGRAM };
static uint32_t g_wstate, g_tries;
static absolute_time_t g_deadline, g_next_poll;

static void cmd(uint32_t c) {
    const uint16_t words[2] = { (uint16_t)(c >> 16), (uint16_t)c };
    n64_bus_write_words(FRAM_CMD, words, 2);
}

static void fram_mode(uint32_t m) {
    if (g_mode == m) return;
    cmd(m);
    sleep_us(50);
    g_mode = m;
}

static uint32_t status(void) {
    uint16_t w[2];
    n64_bus_read_words(CART_SAVE, w, 2);
    return w[1] & 0xFFu;
}

static int lookup(uint32_t maker) {
    for (unsigned i = 0; i < sizeof FRAM_ID / sizeof FRAM_ID[0]; i++)
        if ((maker & 0xFF) == FRAM_ID[i].code) return (int)i;
    return -1;
}

// With no FlashRAM there, the id command's word goes back as it was: an SRAM cart may alias it onto its save
static bool read_id(uint32_t id[2]) {
    uint16_t w[4], keep[2];
    n64_bus_read_words(FRAM_CMD, keep, 2);
    g_mode = 0;
    fram_mode(CMD_ID);
    n64_bus_read_words(CART_SAVE, w, 4);
    id[0] = (uint32_t)w[0] << 16 | w[1];
    id[1] = (uint32_t)w[2] << 16 | w[3];
    if (id[0] == 0x11118001u) return true;
    n64_bus_write_words(FRAM_CMD, keep, 2);
    g_mode = 0;
    return false;
}

// The chip's bytes [off, off + n) into dst, in read-array mode
static void chip_read(uint32_t off, uint8_t *dst, uint32_t n) {
    bool halved = FRAM_ID[g_type].halved;
    uint32_t chunk = halved ? 128 : BLOCK_BYTES;
    fram_mode(CMD_READ);
    while (n) {
        uint32_t in = off % chunk, k = chunk - in < n ? chunk - in : n;
        uint32_t addr = CART_SAVE + (halved ? (off - in) / 2 : off - in);
        static uint8_t blk[BLOCK_BYTES];
        n64_bus_read(addr, blk, chunk);
        memcpy(dst, blk + in, k);
        dst += k, off += k, n -= k;
    }
}

int cart_flashram_identify(void) {
    uint32_t id[2];
    cart_flashram_service_idle();  // the id command would cut a page write short
    bool ok = read_id(id);
    printf("fram   id %08lX %08lX\n", (unsigned long)id[0], (unsigned long)id[1]);
    if (!ok) {
        printf("fram   no FlashRAM answered (expected 11118001); check /WRITE on cart pin 8\n");
        return -1;
    }
    int t = lookup(id[1]);
    if (t < 0) printf("fram   unknown device code %02lX, not reading it\n", (unsigned long)(id[1] & 0xFF));
    else printf("fram   %s\n", FRAM_ID[t].name);
    return t;
}

void cart_flashram_dump(void) {
    static uint8_t buf[BLOCK_BYTES];
    int saved = g_type;
    g_type = cart_flashram_identify();
    if (g_type < 0) {
        g_type = saved;
        printf("N64SAVE 0\n");
        return;
    }
    console_raw_begin("N64SAVE", FRAM_BYTES);
    for (uint32_t off = 0; off < FRAM_BYTES; off += sizeof buf) {
        chip_read(off, buf, sizeof buf);
        fwrite(buf, 1, sizeof buf, stdout);
    }
    console_raw_end();
    g_type = saved;
}

bool cart_flashram_attach(bool probe) {
    g_wbusy = false, g_wstate = W_IDLE, g_tries = 0, g_given_up = 0;
    g_st_gen = 1, g_st_len = 0, g_want_ack = g_want_seq;
    uint32_t id[2];
    g_type = probe && read_id(id) ? lookup(id[1]) : -1;
    g_id[0] = g_type < 0 ? 0 : id[0], g_id[1] = g_type < 0 ? 0 : id[1];
    return g_type >= 0;
}

bool cart_flashram_present(void) { return g_type >= 0; }
const char *cart_flashram_name(void) { return g_type < 0 ? "none" : FRAM_ID[g_type].name; }
void cart_flashram_id(uint32_t id[2]) { id[0] = g_id[0], id[1] = g_id[1]; }

bool __not_in_flash_func(fram_read)(uint32_t off, uint32_t len, uint8_t *dst) {
    if (g_type < 0 || off > FRAM_BYTES || len > FRAM_BYTES - off) return false;
    uint32_t gen = g_st_gen;
    __dmb();
    if (!g_wbusy && !(gen & 1) && off >= g_st_off && off + len <= g_st_off + g_st_len) {
        memcpy(dst, g_stage + (off - g_st_off), len);
        __dmb();
        if (g_st_gen == gen) {
            if (off + 2 * len > g_st_off + g_st_len && off + len < FRAM_BYTES) {  // the next read won't fit: fetch it now
                g_want = off + len;
                __dmb();
                g_want_seq = g_want_seq + 1;
            }
            return true;
        }
    }
    g_want = off;
    __dmb();
    g_want_seq = g_want_seq + 1;
    return false;
}

bool __not_in_flash_func(fram_write)(uint32_t off, uint32_t len, const uint8_t *src) {
    if (g_type < 0 || len != FRAM_PAGE || off % FRAM_PAGE || off >= FRAM_BYTES || g_wbusy) return false;
    memcpy(g_page, src, FRAM_PAGE);
    g_page_off = off;
    __dmb();
    g_wbusy = true;
    return true;
}

void __not_in_flash_func(fram_status)(uint32_t *pending, uint32_t *given_up) {
    *pending = g_wbusy ? 1 : 0;
    *given_up = g_given_up;
}

static void stage_fill(uint32_t off) {
    g_st_gen = g_st_gen | 1;
    __dmb();
    uint32_t n = FRAM_BYTES - off < STAGE ? FRAM_BYTES - off : STAGE;
    chip_read(off, g_stage, n);
    g_st_off = off, g_st_len = n;
    __dmb();
    g_st_gen = g_st_gen + 1;
}

static void erase_start(void) {
    g_st_gen = g_st_gen | 1;  // the staged bytes may be this page's old ones
    cmd(CMD_ERASE_PAGE | g_page_off / FRAM_PAGE);
    cmd(CMD_ERASE_RUN);
    g_mode = 0;
    g_wstate = W_ERASE;
    g_deadline = make_timeout_time_ms(CYCLE_MS);
    g_next_poll = make_timeout_time_us(POLL_US);
}

static void program_start(void) {
    uint16_t w[FRAM_PAGE / 2];
    for (uint32_t i = 0; i < FRAM_PAGE / 2; i++) w[i] = (uint16_t)(g_page[2 * i] << 8 | g_page[2 * i + 1]);
    cmd(CMD_BUFFER);
    n64_bus_write_words(CART_SAVE, w, FRAM_PAGE / 2);
    if (!FRAM_ID[g_type].halved) cmd(CMD_BUFFER);  // libultra's NEW_FLASH sends it again before programming
    cmd(CMD_PROGRAM | g_page_off / FRAM_PAGE);
    g_wstate = W_PROGRAM;
}

static void write_end(bool ok) {
    if (!ok && ++g_tries < TRIES) {
        erase_start();
        return;
    }
    if (!ok) g_given_up = g_given_up + 1;
    g_tries = 0;
    g_wstate = W_IDLE;
    __dmb();
    g_wbusy = false;
}

static void write_verify(void) {
    static uint8_t back[FRAM_PAGE];
    static const uint16_t zero[2];
    cmd(CMD_STATUS);
    n64_bus_write_words(CART_SAVE, zero, 2);  // clears the status, as osFlashClearStatus
    g_mode = 0;
    chip_read(g_page_off, back, FRAM_PAGE);
    write_end(memcmp(back, g_page, FRAM_PAGE) == 0);
}

static void write_step(void) {
    if (g_wstate == W_IDLE) {
        erase_start();
        return;
    }
    if (!time_reached(g_next_poll)) return;
    g_next_poll = make_timeout_time_us(POLL_US);
    uint32_t s = status();
    if (g_wstate == W_ERASE && !(s & ST_ERASE_BUSY)) program_start();
    else if (g_wstate == W_PROGRAM && !(s & ST_WRITE_BUSY)) write_verify();
    else if (time_reached(g_deadline)) write_end(false);
}

void cart_flashram_service(void) {
    if (g_type < 0) return;
    if (g_wbusy) {
        write_step();
        return;
    }
    uint32_t seq = g_want_seq;
    if (seq == g_want_ack) return;
    __dmb();
    uint32_t off = g_want, end = off + CART_CRC_MAX_LEN < FRAM_BYTES ? off + CART_CRC_MAX_LEN : FRAM_BYTES;
    g_want_ack = seq;
    if ((g_st_gen & 1) || off < g_st_off || end > g_st_off + g_st_len) stage_fill(off);
}

void cart_flashram_service_idle(void) {
    absolute_time_t until = make_timeout_time_ms(2 * CYCLE_MS * TRIES);
    while (g_wbusy && !time_reached(until)) cart_flashram_service();
}
