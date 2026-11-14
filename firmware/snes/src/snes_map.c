#include <string.h>
#include "snes_map.h"

#define MB (1u << 20)
#define H(a) hdr[(a) - SNES_HDR_BUS]

static uint32_t at(uint32_t bank, uint32_t addr) { return (bank & 0xFFu) << 16 | (addr & 0xFFFFu); }

static snes_chip_t chip_of(uint8_t t) {
    if (t < 0x03) return SNES_CHIP_NONE;
    if (t == 0xF3) return SNES_CHIP_CX4;
    if (t == 0xF5 || t == 0xF9) return SNES_CHIP_SPC7110;
    switch (t >> 4) {
    case 0x0: return SNES_CHIP_DSP;
    case 0x1: return SNES_CHIP_SUPERFX;
    case 0x2: return SNES_CHIP_OBC1;
    case 0x3: return SNES_CHIP_SA1;
    case 0x4: return SNES_CHIP_SDD1;
    case 0x5: return SNES_CHIP_SRTC;
    default:  return SNES_CHIP_OTHER;
    }
}

// Cart type low nibble: 2 ROM+RAM+battery, 5/6/9/A coprocessor variants with battery.
static bool has_battery(uint8_t t) {
    if (t < 0x03) return t == 0x02;
    uint8_t n = t & 0x0Fu;
    return n == 0x2 || n == 0x5 || n == 0x6 || n == 0x9 || n == 0xA;
}

static uint32_t sram_bytes(const uint8_t *hdr, const snes_cart_t *c) {
    if (!c->battery || c->chip == SNES_CHIP_SA1) return 0;
    uint8_t e;
    if (c->chip == SNES_CHIP_SUPERFX) e = H(0xFFDA) == 0x33 ? H(0xFFBD) : strncmp(c->title, "STARFOX2", 8) ? 5 : 6;
    else e = H(0xFFD8);
    return e >= 1 && e <= 7 ? 1024u << e : 0;
}

void snes_parse_header(const uint8_t *hdr, snes_cart_t *c) {
    memset(c, 0, sizeof *c);
    memcpy(c->hdr, hdr, SNES_HDR_LEN);
    for (int i = 0; i < 21; i++) {
        uint8_t ch = H(0xFFC0 + i);
        c->title[i] = ch >= 0x20 && ch < 0x7F ? (char)ch : '?';
    }
    for (int i = 20; i >= 0 && c->title[i] == ' '; i--) c->title[i] = 0;

    c->map_byte   = H(0xFFD5);
    c->chips      = H(0xFFD6);
    c->complement = (uint16_t)(H(0xFFDC) | H(0xFFDD) << 8);
    c->checksum   = (uint16_t)(H(0xFFDE) | H(0xFFDF) << 8);
    c->header_ok  = c->checksum && (uint32_t)c->checksum + c->complement == 0xFFFFu;

    // OSCR SNES.ino:1182-1193; the first test guards 22-character titles
    uint8_t b = c->map_byte;
    if ((b >> 5) != 1 || c->checksum == 0x3BB0) c->map = SNES_MAP_LOROM;
    else if ((b & 0xEFu) == 0x25) c->map = SNES_MAP_EXHIROM;
    else if (b == 0x3A) c->map = SNES_MAP_HIROM;
    else c->map = b & 1 ? SNES_MAP_HIROM : SNES_MAP_LOROM;

    c->chip    = chip_of(c->chips);
    c->battery = has_battery(c->chips);

    uint8_t rs = H(0xFFD7);
    c->size_ok = rs >= 0x08 && rs <= 0x0D;
    c->rom_declared = c->size_ok ? 1024u << rs : 0;
    c->rom_size = c->rom_declared;

    // OSCR SNES.ino:1201-1223 and 1477-1497
    if (c->chips == 0x43 || c->chips == 0x45) {
        c->map = SNES_MAP_HIROM;
        c->rom_size = c->chips == 0x45 ? 6 * MB : 4 * MB;
        c->size_ok = true;
    } else if (c->chip == SNES_CHIP_CX4) {
        uint8_t n = H(0xFFC9) & 0x0Fu;
        if (n == 2 || n == 3) {
            c->rom_size = n == 3 ? 2 * MB : 3 * MB / 2;
            c->cx4_variant = n == 3;
            c->size_ok = true;
        }
    } else if (c->chip == SNES_CHIP_SPC7110 && c->map == SNES_MAP_HIROM) {
        c->rom_size = c->chips == 0xF9 ? 5 * MB : 3 * MB;
        c->size_ok = true;
    } else if (c->map == SNES_MAP_LOROM && (c->checksum == 0xCC86 || c->checksum == 0xA77B)) {
        c->derby = true;
        c->rom_size = 3 * MB;
        c->size_ok = true;
    }
    if (c->map == SNES_MAP_EXHIROM && c->rom_size > SNES_EXHI_MAX) c->rom_size = SNES_EXHI_MAX;
    if (c->chip == SNES_CHIP_SA1 || !c->size_ok) c->rom_size = 0;

    c->sram_size = sram_bytes(hdr, c);
}

bool snes_romsel_low(uint32_t bus) {
    uint8_t bank = (uint8_t)(bus >> 16);
    if (bank & 0x40u) return (bank & 0xFEu) != 0x7E;
    return (bus & 0x8000u) != 0;
}

uint32_t snes_rom_bus(const snes_cart_t *c, uint32_t off, uint8_t *state) {
    *state = SNES_STATE_ANY;
    switch (c->chip) {
    case SNES_CHIP_SDD1:
        *state = (uint8_t)(off >> 20);
        return at(0xF0 + ((off >> 16) & 0xFu), off);
    case SNES_CHIP_SPC7110:
        if (off < 2 * MB) { *state = 0; return at(0xC0 + (off >> 16), off); }
        *state = off < 4 * MB ? 1 : 2;
        return at((off < 3 * MB ? 0xE0 : 0xF0) + ((off >> 16) & 0xFu), off);
    case SNES_CHIP_CX4:
        *state = 0;
        break;
    default:
        break;
    }
    if (c->map == SNES_MAP_LOROM) {
        uint32_t half = off >> 15;
        uint32_t bank = c->derby ? (half < 0x40 ? half : 0x80 + half - 0x40)
                                 : (c->rom_size > 3 * MB ? 0x80u : 0u) + half;
        return at(bank, 0x8000u | (off & 0x7FFFu));
    }
    if (c->map == SNES_MAP_EXHIROM && off >= 4 * MB) return at(0x40 + ((off - 4 * MB) >> 16), off);
    return at(0xC0 + (off >> 16), off);
}

unsigned snes_state_writes(const snes_cart_t *c, uint8_t state, snes_reg_write_t w[2]) {
    switch (c->chip) {
    case SNES_CHIP_SDD1:
        w[0] = (snes_reg_write_t){ 0x004807, state };
        return 1;
    case SNES_CHIP_SPC7110:
        w[0] = (snes_reg_write_t){ 0x004834, state ? 0xFF : 0x00 };
        w[1] = (snes_reg_write_t){ 0x004833, state == 2 ? 3 : 2 };
        return 2;
    case SNES_CHIP_CX4:
        w[0] = (snes_reg_write_t){ 0x007F52, c->cx4_variant };
        return 1;
    default:
        return 0;
    }
}

uint32_t snes_sram_bus(const snes_cart_t *c, uint32_t off) {
    if (c->chip == SNES_CHIP_SUPERFX) return at(0x70 + (off >> 16), off);
    if (c->map == SNES_MAP_LOROM) return at(0x70 + (off >> 15), off & 0x7FFFu);
    return at((c->map == SNES_MAP_EXHIROM ? 0xB0u : 0x30u) + (off >> 13), 0x6000u | (off & 0x1FFFu));
}

unsigned snes_sram_enable_writes(const snes_cart_t *c, bool on, snes_reg_write_t w[1]) {
    if (c->chip != SNES_CHIP_SPC7110) return 0;
    w[0] = (snes_reg_write_t){ 0x004830, on ? 0x80 : 0x00 };
    return 1;
}

static uint32_t span_sum(const uint32_t *cs, uint32_t from, uint32_t to) {
    uint32_t s = 0;
    for (uint32_t i = from / SNES_CHECK_CHUNK; i < to / SNES_CHECK_CHUNK; i++) s += cs[i];
    return s;
}

// OSCR SNES.ino:1323-1444
uint16_t snes_checksum(const uint32_t *chunk_sum, uint32_t size, uint8_t chips) {
    uint32_t base = SNES_CHECK_CHUNK;
    while (base * 2 <= size) base *= 2;
    uint32_t all = span_sum(chunk_sum, 0, size);
    uint32_t rest = size - base;
    if (!rest) return (uint16_t)all;
    if (size == 3 * MB && chips == 0xF5) return (uint16_t)(2 * all);
    if (size == 5 * MB && chips != 0x55) return (uint16_t)all;
    if (base % rest) return (uint16_t)all;
    return (uint16_t)(span_sum(chunk_sum, 0, base) + base / rest * span_sum(chunk_sum, base, size));
}

const char *snes_map_name(snes_map_t m) {
    static const char *const k[] = { "LoROM", "HiROM", "ExHiROM" };
    return k[m];
}

const char *snes_chip_name(snes_chip_t c) {
    static const char *const k[] = { "none", "DSP", "SuperFX", "OBC1", "SA-1", "S-DD1", "S-RTC", "CX4",
                                     "SPC7110", "other" };
    return k[c];
}
