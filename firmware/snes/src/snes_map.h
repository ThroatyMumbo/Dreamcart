// SNES cart decoding, pure C: header, file offset -> 24-bit bus address, the console's /ROMSEL decode, checksum.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SNES_HDR_BUS     0x00FFB0u      // 00:FFB0-FFFF; the cart's decoder puts its own header there
#define SNES_HDR_LEN     80u
#define SNES_EXHI_MAX    0x7E0000u      // C0-FF then 40-7D; 7E-7F are WRAM
#define SNES_STATE_ANY   0xFFu          // no chip register state needed
#define SNES_CHECK_CHUNK (128u << 10)   // checksum bookkeeping granularity

typedef enum { SNES_MAP_LOROM, SNES_MAP_HIROM, SNES_MAP_EXHIROM } snes_map_t;

typedef enum {
    SNES_CHIP_NONE, SNES_CHIP_DSP, SNES_CHIP_SUPERFX, SNES_CHIP_OBC1, SNES_CHIP_SA1, SNES_CHIP_SDD1,
    SNES_CHIP_SRTC, SNES_CHIP_CX4, SNES_CHIP_SPC7110, SNES_CHIP_OTHER,
} snes_chip_t;

typedef struct { uint32_t bus; uint8_t val; } snes_reg_write_t;

typedef struct {
    uint8_t     hdr[SNES_HDR_LEN];
    char        title[22];
    uint8_t     map_byte, chips;
    snes_map_t  map;
    snes_chip_t chip;
    bool        header_ok;      // checksum + complement == FFFF, checksum not 0000
    bool        size_ok;        // ROM size byte in range
    bool        battery;
    bool        derby;          // Derby Stallion '96: 00-3F then 80-9F
    uint8_t     cx4_variant;    // 00:7F52 while reading: 0 for X2, 1 for X3
    uint16_t    checksum, complement;
    uint32_t    rom_declared;   // from the header byte alone
    uint32_t    rom_size;       // what Maple serves, 0 when nothing is servable
    uint32_t    sram_size;      // 0 without battery, or when SA-1
} snes_cart_t;

void snes_parse_header(const uint8_t *hdr, snes_cart_t *c);

// The S-CPU's decode: low in 40-7D and C0-FF, and for A15=1 in 00-3F and 80-BF.
bool snes_romsel_low(uint32_t bus);

// ROM: off must be < rom_size. *state is the chip register state the read needs, or SNES_STATE_ANY.
uint32_t snes_rom_bus(const snes_cart_t *c, uint32_t off, uint8_t *state);
unsigned snes_state_writes(const snes_cart_t *c, uint8_t state, snes_reg_write_t w[2]);

// SRAM: off must be < sram_size. Accesses stay inside one 8 KB piece of offsets.
uint32_t snes_sram_bus(const snes_cart_t *c, uint32_t off);
unsigned snes_sram_enable_writes(const snes_cart_t *c, bool on, snes_reg_write_t w[1]);

// OSCR's checksum for a dump of size bytes (a multiple of SNES_CHECK_CHUNK), from per-chunk byte sums.
uint16_t snes_checksum(const uint32_t *chunk_sum, uint32_t size, uint8_t chips);

const char *snes_map_name(snes_map_t m);
const char *snes_chip_name(snes_chip_t c);
