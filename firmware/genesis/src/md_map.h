// Mega Drive cart decoding, pure C: header, file offset -> word address, save layout, checksum.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MD_HDR_BUS      0x000100u       // byte address of the header
#define MD_HDR_LEN      256u
#define MD_LOCK_BUS     0x200100u       // the cart locked onto Sonic & Knuckles, its header
#define MD_PIECE        0x80000u        // every mapping is linear inside an aligned 512 KB of offsets
#define MD_SAVE_MAX     (64u << 10)
#define MD_ROM_MAX      (16u << 20)
#define MD_STATE_ANY    0xFFu
#define MD_SRAM_SWITCH  (0xA130F1u >> 1) // word address in the TIME range; S&K's Sonic 2 patch switch too
#define MD_SSF2_BANK(n) ((0xA130F1u + 2u * (n)) >> 1)  // SSF2 mapper: window n's 512 KB page, n 1-7
#define MD_S2_PATCH     0x40000u        // Sonic 2's extra ROM in S&K, after A130F1 = 1

typedef enum { MD_MAP_LINEAR, MD_MAP_SSF2 } md_map_t;
typedef enum { MD_SAVE_NONE, MD_SAVE_ODD, MD_SAVE_EVEN, MD_SAVE_WORD, MD_SAVE_EEPROM, MD_SAVE_UNKNOWN } md_save_t;
typedef enum { MD_SNK_NONE, MD_SNK_ALONE, MD_SNK_S1, MD_SNK_S2, MD_SNK_S3, MD_SNK_OTHER, MD_SNK_PROBE } md_snk_t;

typedef struct { uint32_t word; uint16_t val; } md_reg_write_t;

typedef struct {
    uint8_t   hdr[MD_HDR_LEN];      // 0x100-0x1FF
    char      system[17], title[49], serial[15];
    uint16_t  hdr_checksum;         // 0x18E as read
    uint16_t  checksum;             // after OSCR's fix-ups; what 'v' expects
    uint32_t  rom_declared;         // ROM end + 1 from the header
    uint32_t  rom_size;             // this cart alone, after fix-ups
    uint32_t  served;               // what Maple serves: rom_size, plus the lock-on parts; 0 for nothing
    bool      size_unknown;         // header size out of range; md_set_rom_size after a mirror scan
    md_map_t  map;
    bool      sram_switch;          // 2-4 MB: ROM and SRAM share 0x200000 (Phantasy Star IV)
    bool      svp, is32x;
    md_save_t save;
    uint32_t  save_word;            // first word address of the save
    uint32_t  save_size;            // bytes Maple serves: one per word for 8-bit SRAM
    uint32_t  save_hdr_base, save_hdr_end;
    uint32_t  eeprom_size;          // bytes, for 'i' only
    md_snk_t  snk;
    char      lock_title[49], lock_serial[15];
    uint32_t  lock_size;
    uint16_t  lock_checksum;
} md_cart_t;

// first_word is ROM word 0, for OSCR's Codemasters check. Leaves snk at MD_SNK_PROBE for Sonic & Knuckles.
void md_parse_header(const uint8_t *hdr, uint16_t first_word, md_cart_t *c);
void md_set_rom_size(md_cart_t *c, uint32_t size);

// The header read at MD_LOCK_BUS; settles snk and served.
void md_parse_lockon(const uint8_t *lock_hdr, md_cart_t *c);

// ROM: off < served, even. *state is the bank register state the read needs, or MD_STATE_ANY.
uint32_t md_rom_bus(const md_cart_t *c, uint32_t off, uint8_t *state);
unsigned md_state_writes(const md_cart_t *c, uint8_t state, md_reg_write_t w[1]);

// Save: off < save_size. WORD saves take even offsets.
bool md_save_served(const md_cart_t *c);
uint32_t md_save_word(const md_cart_t *c, uint32_t off);
uint32_t md_save_bus(const md_cart_t *c);      // byte address of the first save byte, for printing

// Adds n bytes (even) of big-endian words to sum; the header checksum covers each part from 0x200 on.
uint16_t md_sum_words(const uint8_t *p, uint32_t n, uint16_t sum);

const char *md_save_name(md_save_t s);
const char *md_snk_name(md_snk_t s);
