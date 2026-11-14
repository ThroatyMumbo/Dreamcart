#include <stdio.h>
#include <string.h>
#include "console.h"
#include "md_bus.h"
#include "md_power.h"
#include "md_cart.h"

#define SCAN_WORDS 9u       // OSCR MD.ino:1321 compares the first 9 words against each candidate mirror
#define SAVE_CHUNK 128u     // words per save burst

static md_cart_t g_cart;
static int g_state = -1;    // bank register state last written; -1 unknown

static void apply(const md_reg_write_t *w, unsigned n) {
    for (unsigned i = 0; i < n; i++) md_bus_reg_write(w[i].word, w[i].val);
}

static void enter(uint8_t state) {
    if (state == MD_STATE_ANY || state == g_state) return;
    md_reg_write_t w[1];
    apply(w, md_state_writes(&g_cart, state, w));
    g_state = state;
}

// OSCR MD.ino:1319-1326: the first mirror of ROM word 0..8 on a 128 KB step, else 4 MB
static uint32_t mirror_scan(void) {
    uint8_t first[2 * SCAN_WORDS], at[2 * SCAN_WORDS];
    md_bus_read(0, first, SCAN_WORDS);
    uint32_t words;
    for (words = 0x10000u; words < 0x200000u; words += 0x10000u) {
        md_bus_read(words, at, SCAN_WORDS);
        if (!memcmp(first, at, sizeof first)) break;
    }
    return words * 2;
}

void md_cart_detect(void) {
    uint8_t h[MD_HDR_LEN], w0[2];
    md_bus_read(MD_HDR_BUS >> 1, h, MD_HDR_LEN / 2);
    md_bus_read(0, w0, 1);
    md_parse_header(h, (uint16_t)(w0[0] << 8 | w0[1]), &g_cart);
    g_state = -1;

    if (g_cart.snk == MD_SNK_PROBE) {
        md_bus_read(MD_LOCK_BUS >> 1, h, MD_HDR_LEN / 2);
        md_parse_lockon(h, &g_cart);
        if (g_cart.snk == MD_SNK_S2) md_bus_reg_write(0xA130F0u >> 1, 1);     // OSCR MD.ino:1098
    }
    if (g_cart.size_unknown) md_set_rom_size(&g_cart, mirror_scan());
    if (g_cart.map == MD_MAP_SSF2) enter(7);
    if (g_cart.sram_switch || md_save_served(&g_cart)) md_bus_reg_write(MD_SRAM_SWITCH, 0);  // MD.ino:1465
}

const md_cart_t *md_cart(void) { return &g_cart; }

static void rom_piece(uint32_t o, uint8_t *d, uint32_t k) {
    uint8_t state, t[2];
    uint32_t w = md_rom_bus(&g_cart, o & ~1u, &state);
    enter(state);
    if (o & 1) { md_bus_read(w++, t, 1); *d++ = t[1]; k--; }
    md_bus_read(w, d, k / 2);
    if (k & 1) { md_bus_read(w + k / 2, t, 1); d[k - 1] = t[0]; }
}

void md_cart_read(uint32_t off, void *dst, uint32_t n) {
    uint8_t *d = dst;
    uint32_t span = g_cart.served;
    if (!span) { memset(d, 0xFF, n); return; }
    while (n) {
        uint32_t o = off % span;
        uint32_t k = MD_PIECE - o % MD_PIECE;
        if (k > span - o) k = span - o;
        if (k > n) k = n;
        rom_piece(o, d, k);
        off += k;
        d += k;
        n -= k;
    }
}

// The switch at A130F1 maps SRAM over ROM at 0x200000 only for the transfer.
static void save_xfer(uint32_t off, uint8_t *dst, const uint8_t *src, uint32_t n) {
    static uint8_t raw[2 * SAVE_CHUNK];
    static uint16_t out[SAVE_CHUNK];
    const md_save_t s = g_cart.save;
    const uint32_t per = s == MD_SAVE_WORD ? 2 : 1;
    md_bus_reg_write(MD_SRAM_SWITCH, 1);
    while (n) {
        uint32_t words = n / per < SAVE_CHUNK ? n / per : SAVE_CHUNK;
        if (!words) break;
        uint32_t w = md_save_word(&g_cart, off);
        if (dst) {
            md_bus_read(w, raw, words);
            if (s == MD_SAVE_WORD) memcpy(dst, raw, 2 * words);
            else for (uint32_t i = 0; i < words; i++) dst[i] = raw[2 * i + (s == MD_SAVE_ODD)];
            dst += words * per;
        } else {
            for (uint32_t i = 0; i < words; i++)
                out[i] = s == MD_SAVE_WORD ? (uint16_t)(src[2 * i] << 8 | src[2 * i + 1])
                                           : (uint16_t)(src[i] << 8 | src[i]);
            // both lanes carry the byte, so either write strobe the cart decodes takes it
            md_bus_write(w, out, words, MD_STROBE_CE | MD_STROBE_LWR | MD_STROBE_UWR);
            src += words * per;
        }
        off += words * per;
        n -= words * per;
    }
    md_bus_reg_write(MD_SRAM_SWITCH, 0);
}

bool md_cart_save_read(uint32_t off, uint8_t *dst, uint32_t n) {
    save_xfer(off, dst, NULL, n);
    return true;
}

bool md_cart_save_write(uint32_t off, const uint8_t *src, uint32_t n) {
    save_xfer(off, NULL, src, n);
    return true;
}

void md_cart_print(void) {
    const md_cart_t *c = &g_cart;
    printf("header at 000100:");
    for (unsigned i = 0; i < MD_HDR_LEN; i++) {
        if (i % 16 == 0) printf("\n  %06X  ", (unsigned)(MD_HDR_BUS + i));
        printf("%02X ", c->hdr[i]);
    }
    printf("\n\ncart det %s\n", md_cart_present() ? "low: cart in" : "high: no cart, or B32 is not a detect");
    printf("system   \"%s\"%s\n", c->system, c->is32x ? "  32X cart: the emulator has no 32X" : "");
    printf("title    \"%s\"\n", c->title);
    printf("serial   \"%s\"%s\n", c->serial, c->svp ? "  SVP: ROM served, the emulator has no SVP" : "");
    printf("checksum %04X", c->hdr_checksum);
    if (c->checksum != c->hdr_checksum) printf(", %04X per no-intro", c->checksum);
    console_print_size("\nrom      declared ", c->rom_declared);
    if (c->rom_size) console_print_size(", cart ", c->rom_size);
    else printf(", size unknown");
    if (c->served) console_print_size(", serving ", c->served);
    printf("%s%s\n", c->map == MD_MAP_SSF2 ? ", SSF2 banks" : "", c->sram_switch ? ", ROM/SRAM switch" : "");
    if (c->snk != MD_SNK_NONE) {
        printf("lock-on  %s", md_snk_name(c->snk));
        if (c->snk >= MD_SNK_S1 && c->snk <= MD_SNK_OTHER) {
            printf(": \"%s\" %s checksum %04X", c->lock_title, c->lock_serial, c->lock_checksum);
            console_print_size(", ", c->lock_size);
            if (c->snk == MD_SNK_S2) printf(" + 256 KB patch");
        }
        printf("\n");
    }
    printf("save     %s", md_save_name(c->save));
    if (md_save_served(c)) printf(", %lu bytes at %06lX", (unsigned long)c->save_size, (unsigned long)md_save_bus(c));
    else if (c->save == MD_SAVE_EEPROM) printf(" (%u bytes): not supported, no save over Maple", (unsigned)c->eeprom_size);
    else if (c->save == MD_SAVE_UNKNOWN) printf(" %06lX-%06lX: no save over Maple", (unsigned long)c->save_hdr_base,
                                                (unsigned long)c->save_hdr_end);
    if (c->snk >= MD_SNK_S1 && c->snk <= MD_SNK_OTHER) printf("; the lock-on cart's save is not served");
    printf("\n");
    if (!c->served) printf("no usable ROM size: Maple serves nothing\n");
}
