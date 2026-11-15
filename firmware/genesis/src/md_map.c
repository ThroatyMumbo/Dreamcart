#include <string.h>
#include "md_map.h"

#define MB (1u << 20)
#define H(a) hdr[(a) - MD_HDR_BUS]

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)be16(p) << 16 | be16(p + 2); }

static void text(char *dst, const uint8_t *src, unsigned n) {
    for (unsigned i = 0; i < n; i++) dst[i] = src[i] >= 0x20 && src[i] < 0x7F ? (char)src[i] : '?';
    dst[n] = 0;
    for (int i = (int)n - 1; i >= 0 && dst[i] == ' '; i--) dst[i] = 0;
}

// OSCR copyToRomName_MD: the overseas title with everything outside 0-9 and A-z dropped, 16 characters
static void oscr_name(char out[17], const uint8_t *hdr) {
    unsigned n = 0;
    for (unsigned i = 0; i < 48 && n < 16; i++) {
        uint8_t ch = H(0x150 + i);
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'z')) out[n++] = (char)ch;
    }
    out[n] = 0;
}

typedef struct { uint32_t size; uint16_t sum; uint32_t new_size; uint16_t new_sum; } size_fix_t;

// OSCR MD.ino:824-952, no-intro sizes and checksums by header size and checksum
static const size_fix_t SIZE_FIX[] = {
    { 0x400000, 0xCE25, 0x500000, 0xCE25 }, { 0x400000, 0xE41D, 0x500000, 0xE41D },
    { 0x400000, 0xE017, 0x500000, 0xE017 }, { 0x400000, 0x0000, 0xEAF2F4, 0x0000 },
    { 0x400000, 0xBCBF, 0xEA0000, 0xBCBF }, { 0x400000, 0x6E1E, 0xEA0000, 0x6E1E },
    { 0x300000, 0xBC5F, 0x400000, 0xBC5F }, { 0x300000, 0x3CDD, 0x400000, 0x3CDD },
    { 0x300000, 0x44AD, 0x400000, 0x44AD }, { 0x300000, 0x2D9A, 0x400000, 0x2D9A },
    { 0x300000, 0x5648, 0x400000, 0x5648 }, { 0x300000, 0x0A29, 0x400000, 0x0A29 },
    { 0x300000, 0x7651, 0x400000, 0x7651 }, { 0x300000, 0x74CA, 0x400000, 0x74CA },
    { 0x200000, 0x2078, 0x200000, 0x9877 }, { 0x200000, 0xAE95, 0x200000, 0x56A0 },
    { 0x180000, 0xFFE2, 0x200000, 0xFFE2 }, { 0x180000, 0xF418, 0x200000, 0xF418 },
    { 0x180000, 0xF71D, 0x200000, 0xF71D }, { 0x180000, 0xA884, 0x200000, 0xA884 },
    { 0x180000, 0x7D68, 0x200000, 0x7D68 }, { 0x180000, 0x030D, 0x200000, 0x030D },
    { 0x180000, 0xE975, 0x200000, 0xE975 },
    { 0x100000, 0xCDF5, 0x400000, 0x603A }, { 0x100000, 0xF85F, 0x200000, 0x6965 },
    { 0x100000, 0x4581, 0x400000, 0x0694 },
    { 0x0C0000, 0x9D79, 0x100000, 0x9D79 },
    { 0x080000, 0x06C1, 0x200000, 0x8473 }, { 0x080000, 0x5B3A, 0x200000, 0x5613 },
    { 0x080000, 0xD07D, 0x100000, 0xF204 }, { 0x080000, 0x95C9, 0x100000, 0x95C9 },
    { 0x080000, 0x9144, 0x100000, 0x9144 }, { 0x080000, 0xB8D4, 0x100000, 0xB8D4 },
    { 0x080000, 0xC422, 0x080000, 0xC751 }, { 0x080000, 0x0C6A, 0x080000, 0xE1AA },
    { 0x080000, 0xA760, 0x080000, 0x97CD }, { 0x080000, 0x1404, 0x080000, 0x53B9 },
    { 0x040000, 0x8BC6, 0x100000, 0x8BC6 }, { 0x040000, 0xB344, 0x100000, 0xB344 },
    { 0x020000, 0x7E50, 0x100000, 0xD074 }, { 0x020000, 0x168B, 0x100000, 0xCEE0 },
};

typedef struct { bool by_name; const char *prefix; uint16_t sum; uint32_t new_size; uint16_t new_sum; } id_fix_t;

// OSCR MD.ino:954-1051, applied in order to the already fixed checksum; new_size 0 keeps the size
static const id_fix_t ID_FIX[] = {
    { false, "GM T-44013 ", 0xFFFF, 0x0A0000, 0xC560 },
    { true,  "GMT5604600jJ", 0xFFFF, 0, 0x6BAE },
    { false, "SF-001", 0x3E08, 0x400000, 0x3E08 },
    { false, "SF-002", 0x12B0, 0, 0x45C6 },
    { false, "GM 10101010", 0xC439, 0x100000, 0x21B0 },
    { false, "MU REMUTE01", 0x0000, 0x400000, 0xB55C },
    { false, "GM REMUTE02", 0x0000, 0x400000, 0x5426 },
    { false, "GM HHARVYSG", 0x0000, 0x100000, 0xD9D2 },
    { false, "GM T-107036", 0x0000, 0, 0xAA28 },
    { false, "GM 00000000-43", 0x0000, 0x400000, 0x921B },
    { false, "GM 00000000-00", 0x1E0C, 0x400000, 0xE7E5 },
    { false, "GM 00000000-00", 0x6BD5, 0x400000, 0x1FEA },
    { false, "GM 00000005-00", 0x9F34, 0x400000, 0xA094 },
    { false, "GM 00000005-00", 0x0E9B, 0x400000, 0x6B4B },
    { false, "GM T-574323-00", 0xAEDD, 0x400000, 0xAEDD },
    { false, "GM MK-0000 -00", 0xC536, 0x200000, 0xFAB1 },
    { false, "GM CSET0001-02", 0x0000, 0, 0xE3A9 },
    { false, "1774          ", 0x0000, 0x400000, 0x6E34 },
    { false, "JN-20160131-03", 0x0000, 0x400000, 0x8040 },
    { true,  "ROMEOWJULICAT", 0x0000, 0x200000, 0xB094 },
};

typedef struct { uint16_t sum; uint16_t bytes; } eeprom_id_t;

// OSCR MD.ino:47-92 eepid, size in bytes
static const eeprom_id_t EEPROM_ID[] = {
    { 0x5B9F, 0x100 }, { 0x694F, 0x100 }, { 0xBFA9, 0x100 }, { 0x16B2, 0x100 }, { 0xCC3F, 0x100 },
    { 0x8AE1, 0x100 }, { 0xDB97, 0x100 }, { 0x7651, 0x100 }, { 0xDFE4, 0x100 }, { 0x3DE6, 0x800 },
    { 0xCB78, 0x2000 }, { 0x6DD9, 0x2000 }, { 0xAD23, 0x80 }, { 0xEA80, 0x80 }, { 0x760F, 0x80 },
    { 0x95E7, 0x80 }, { 0x0000, 0x80 }, { 0x7270, 0x80 }, { 0xBACC, 0x80 }, { 0xB939, 0x80 },
    { 0x487C, 0x80 }, { 0x740D, 0x80 }, { 0x0278, 0x80 }, { 0x9D79, 0x80 }, { 0x8512, 0x80 },
    { 0xA107, 0x80 }, { 0x246A, 0x80 }, { 0x5807, 0x80 }, { 0x2799, 0x80 }, { 0xFA57, 0x80 },
    { 0x8B9F, 0x80 }, { 0x7E65, 0x400 }, { 0x9A5C, 0x2000 }, { 0xC4EE, 0x2000 }, { 0x7E50, 0x800 },
    { 0x165E, 0x800 }, { 0x168B, 0x400 }, { 0x12C1, 0x2000 },
};

typedef struct { uint16_t sum; md_save_t save; uint32_t end; } save_fix_t;

// OSCR MD.ino:1231-1270, carts with a missing or bad save header; all start at 0x200001
static const save_fix_t SAVE_FIX[] = {
    { 0xC2DB, MD_SAVE_ODD, 0x200FFF },
    { 0xD7B6, MD_SAVE_ODD, 0x203FFF }, { 0xFE3E, MD_SAVE_ODD, 0x203FFF }, { 0xFDAD, MD_SAVE_ODD, 0x203FFF },
    { 0x632E, MD_SAVE_ODD, 0x203FFF }, { 0xD2BA, MD_SAVE_ODD, 0x203FFF }, { 0x44FE, MD_SAVE_ODD, 0x203FFF },
    { 0xDB5E, MD_SAVE_WORD, 0x207FFF }, { 0x3428, MD_SAVE_WORD, 0x207FFF }, { 0x43EE, MD_SAVE_WORD, 0x207FFF },
    { 0xBF72, MD_SAVE_ODD, 0x20FFFF }, { 0x72EF, MD_SAVE_ODD, 0x20FFFF }, { 0xD723, MD_SAVE_ODD, 0x20FFFF },
    { 0x06C1, MD_SAVE_ODD, 0x20FFFF }, { 0xDB17, MD_SAVE_ODD, 0x20FFFF }, { 0x5B3A, MD_SAVE_ODD, 0x20FFFF },
    { 0x2CF2, MD_SAVE_ODD, 0x20FFFF }, { 0xE9B1, MD_SAVE_ODD, 0x20FFFF }, { 0xEEE8, MD_SAVE_ODD, 0x20FFFF },
};

#define COUNT(a) (sizeof (a) / sizeof (a)[0])

static void set_save(md_cart_t *c, md_save_t s, uint32_t base, uint32_t end) {
    c->save_hdr_base = base;
    c->save_hdr_end = end;
    if (end < base) { c->save = MD_SAVE_UNKNOWN; return; }
    uint32_t size;
    if (s == MD_SAVE_ODD) size = (end - base + 2) / 2;
    else if (s == MD_SAVE_EVEN) size = (end - base + 1) / 2;
    else size = end - base + (base & 1 ? 2 : 1);
    if (!size || size > MD_SAVE_MAX || (s == MD_SAVE_WORD && size & 1)) { c->save = MD_SAVE_UNKNOWN; return; }
    c->save = s;
    c->save_word = base >> 1;
    c->save_size = size;
}

// OSCR MD.ino:1159-1228
static void header_save(md_cart_t *c, const uint8_t *hdr) {
    uint16_t type = be16(&H(0x1B2));
    uint32_t base = be32(&H(0x1B4)), end = be32(&H(0x1B8));
    if (type == 0xF820 || type == 0xF840) {
        if (base == 0x20000020u && end == 0x00010020u) { base = 0x200001; end = 0x203FFF; }    // Psy-o-blade
        if (base == 0x200001 || base == 0x300001 || base == 0x3C0001) set_save(c, MD_SAVE_ODD, base, end);
        else if (base == 0x200000) set_save(c, MD_SAVE_EVEN, base, end);
        else { c->save = MD_SAVE_UNKNOWN; c->save_hdr_base = base; c->save_hdr_end = end; }
    } else if (type == 0xE020) {
        if (base == 0x200001 || base == 0x200000) set_save(c, MD_SAVE_WORD, base, end);
        else if (base != 0x3FFC00) { c->save = MD_SAVE_UNKNOWN; c->save_hdr_base = base; c->save_hdr_end = end; }
    } else if (type == 0xE840) {
        c->save = MD_SAVE_EEPROM;
    }
}

static void derive(md_cart_t *c) {
    c->map = c->rom_size > 4 * MB ? MD_MAP_SSF2 : MD_MAP_LINEAR;
    c->sram_switch = c->rom_size > 2 * MB && c->rom_size < 4 * MB;
    c->served = c->rom_size <= MD_ROM_MAX ? c->rom_size : 0;
}

void md_parse_header(const uint8_t *hdr, uint16_t first_word, md_cart_t *c) {
    memset(c, 0, sizeof *c);
    memcpy(c->hdr, hdr, MD_HDR_LEN);
    text(c->system, &H(0x100), 16);
    text(c->title, &H(0x150), 48);
    if (!c->title[0]) text(c->title, &H(0x120), 48);
    text(c->serial, &H(0x180), 14);
    char name[17];
    oscr_name(name, hdr);

    c->hdr_checksum = c->checksum = be16(&H(0x18E));
    c->rom_declared = be32(&H(0x1A4)) + 1;
    c->rom_size = c->rom_declared;
    c->is32x = !memcmp(&H(0x104), " 32X", 4);
    c->svp = !memcmp(&H(0x180), "GM MK-1229 ", 11) || !memcmp(&H(0x180), "GM G-7001  ", 11);

    for (unsigned i = 0; i < COUNT(SIZE_FIX); i++) {
        if (SIZE_FIX[i].size != c->rom_size || SIZE_FIX[i].sum != c->checksum) continue;
        c->rom_size = SIZE_FIX[i].new_size;
        c->checksum = SIZE_FIX[i].new_sum;
        break;
    }
    for (unsigned i = 0; i < COUNT(ID_FIX); i++) {
        const id_fix_t *f = &ID_FIX[i];
        const char *field = f->by_name ? name : (const char *)&H(0x180);
        if (f->sum != c->checksum || strncmp(field, f->prefix, strlen(f->prefix))) continue;
        if (f->new_size) c->rom_size = f->new_size;
        c->checksum = f->new_sum;
    }

    if (!memcmp(&H(0x180), "GM MK-1563 -00", 14) && c->checksum == 0xDFB3) c->snk = MD_SNK_PROBE;

    // OSCR matches these against its fixed checksum, which misses entries it lists; the header's is meant
    uint16_t sum = c->hdr_checksum;
    for (unsigned i = 0; i < COUNT(EEPROM_ID); i++) {
        if (EEPROM_ID[i].sum != sum) continue;
        c->save = MD_SAVE_EEPROM;
        c->eeprom_size = EEPROM_ID[i].bytes;
        break;
    }
    if (sum == 0x0000 && be16(&H(0x1B2)) != 0xE840) c->save = MD_SAVE_NONE;        // MD.ino:1126
    if ((sum == 0x165E || sum == 0x168B) && first_word != 0x444E) c->save = MD_SAVE_NONE;   // MD.ino:1138
    if (c->save != MD_SAVE_EEPROM) {
        c->eeprom_size = 0;
        if (be16(&H(0x1B0)) == 0x5241) {
            header_save(c, hdr);
        } else {
            for (unsigned i = 0; i < COUNT(SAVE_FIX); i++) {
                if (SAVE_FIX[i].sum != sum) continue;
                set_save(c, SAVE_FIX[i].save, 0x200001, SAVE_FIX[i].end);
                break;
            }
        }
    }

    c->size_unknown = c->rom_size < 0x8000 || c->rom_size > 0xEAF400;       // MD.ino:1319
    if (c->size_unknown) c->rom_size = 0;
    derive(c);
}

void md_set_rom_size(md_cart_t *c, uint32_t size) {
    c->rom_size = size;
    c->size_unknown = false;
    derive(c);
}

// OSCR MD.ino:1053-1108
void md_parse_lockon(const uint8_t *lock_hdr, md_cart_t *c) {
    const uint8_t *hdr = lock_hdr;      // same layout as the main header, 2 MB up
    if (memcmp(&H(0x100), "SEGA MEGA DRIVE ", 16) && memcmp(&H(0x100), "SEGA GENESIS    ", 16)) {
        c->snk = MD_SNK_ALONE;
        return;
    }
    text(c->lock_title, &H(0x150), 48);
    text(c->lock_serial, &H(0x180), 14);
    c->lock_checksum = be16(&H(0x18E));
    c->lock_size = be32(&H(0x1A4)) + 1;
    const char *id = (const char *)&H(0x180);
    if (!memcmp(id, "GM 00001009-0", 13) || !memcmp(id, "GM 00004049-0", 13)) c->snk = MD_SNK_S1;
    else if (!memcmp(id, "GM 00001051-0", 13) && id[13] >= '0' && id[13] <= '2') c->snk = MD_SNK_S2;
    else if (!memcmp(id, "GM MK-1079 -00", 14)) c->snk = MD_SNK_S3;
    else c->snk = MD_SNK_OTHER;

    // the lock-on cart sits in the 2 MB window above S&K, and Sonic 2's patch at 0x300000
    uint32_t window = 4 * MB - c->rom_size;
    if (c->lock_size > window || c->lock_size < 2) c->lock_size = window;
    c->served = c->rom_size + c->lock_size;
    if (c->snk == MD_SNK_S2 && c->served + MD_S2_PATCH <= 4 * MB) c->served += MD_S2_PATCH;
}

static uint32_t window_word(uint32_t off) { return (0x380000u | (off & (MD_PIECE - 1))) >> 1; }

uint32_t md_rom_bus(const md_cart_t *c, uint32_t off, uint8_t *state) {
    *state = MD_STATE_ANY;
    if (c->map == MD_MAP_SSF2 && off >= 7 * MD_PIECE) {
        *state = (uint8_t)(off / MD_PIECE);
        return window_word(off);
    }
    return off >> 1;
}

// OSCR MD.ino:1403-1447: bank 7, 0xA130FF, pages 512 KB into window 0x380000
unsigned md_state_writes(const md_cart_t *c, uint8_t state, md_reg_write_t w[1]) {
    if (c->map != MD_MAP_SSF2 || state == MD_STATE_ANY) return 0;
    w[0] = (md_reg_write_t){ MD_SSF2_BANK(7), state };
    return 1;
}

bool md_save_served(const md_cart_t *c) {
    return c->save == MD_SAVE_ODD || c->save == MD_SAVE_EVEN || c->save == MD_SAVE_WORD;
}

uint32_t md_save_word(const md_cart_t *c, uint32_t off) {
    return c->save_word + (c->save == MD_SAVE_WORD ? off >> 1 : off);
}

uint32_t md_save_bus(const md_cart_t *c) { return c->save_word << 1 | (c->save == MD_SAVE_ODD); }

uint16_t md_sum_words(const uint8_t *p, uint32_t n, uint16_t sum) {
    for (uint32_t i = 0; i + 1 < n; i += 2) sum = (uint16_t)(sum + be16(p + i));
    return sum;
}

const char *md_save_name(md_save_t s) {
    static const char *const k[] = { "none", "SRAM, odd bytes", "SRAM, even bytes", "SRAM, 16-bit",
                                     "serial EEPROM", "unknown layout" };
    return k[s];
}

const char *md_snk_name(md_snk_t s) {
    static const char *const k[] = { "", "S&K alone", "S&K + Sonic 1", "S&K + Sonic 2", "S&K + Sonic 3",
                                     "S&K + other", "S&K, lock-on not probed" };
    return k[s];
}
