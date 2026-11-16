// FlashRAM after libultra's osFlash* and ares Cartridge::Flash, with erase and program done at their execute commands
// (0x78, 0xA5) as the chips do, and never busy.
#include "flashram.h"
#include "os.h"
#include "tmpl.h"

#define RDRAM_SIZE RDRAM_BYTES

enum { M_IDLE, M_STATUS, M_READ, M_WRITE };

static uint8_t *img;
static uint32_t id_type, id_maker, offset, mode, status_lo, chip_erase;
static uint8_t buf[FLASH_PAGE];
static int halved;  // libultra's OLD_FLASH: page n's bytes at n * 64 of address
void (*flash_on_page)(uint32_t page);

void flash_load(uint8_t *image, uint32_t type, uint32_t maker) {
    img = image;
    id_type = type, id_maker = maker;
    halved = maker == 0x00c2001eu || maker == 0x00c20001u || maker == 0x00c20000u;
    mode = M_IDLE, status_lo = 0, offset = 0, chip_erase = 0;
}

int flash_present(void) { return img != 0; }

int flash_claims(uint32_t phys) { return img && phys >= FLASH_BASE && phys < 0x10000000u; }

static uint32_t status_hi(void) { return (id_type & ~0xFFu) | status_lo; }

uint32_t flash_rd(uint32_t phys) { return phys & 4 ? id_maker : status_hi(); }

static void changed(uint32_t page) {
    if (flash_on_page) flash_on_page(page);
}

void flash_wr(uint32_t phys, uint32_t v) {
    if ((phys & 0x1FFFF) != 0x10000) return;  // the status register: libultra's clear, which nothing reads back
    uint32_t page = v & 0xFFFF;
    switch (v >> 24) {
    case 0x3C: chip_erase = 1; return;
    case 0x4B: chip_erase = 0, offset = page * FLASH_PAGE; return;
    case 0x78:
        if (chip_erase) {
            for (uint32_t i = 0; i < FLASH_BYTES; i++) img[i] = 0xFF;
            for (uint32_t p = 0; p < FLASH_BYTES / FLASH_PAGE; p++) changed(p);
        } else if (offset < FLASH_BYTES) {
            for (uint32_t i = 0; i < FLASH_PAGE; i++) img[offset + i] = 0xFF;
            changed(offset / FLASH_PAGE);
        }
        chip_erase = 0, mode = M_STATUS, status_lo = 0x08;
        return;
    case 0xA5:
        offset = page * FLASH_PAGE;
        if (offset < FLASH_BYTES) {
            for (uint32_t i = 0; i < FLASH_PAGE; i++) img[offset + i] = buf[i];
            changed(page);
        }
        mode = M_STATUS, status_lo = 0x04;
        return;
    case 0xB4: mode = M_WRITE; return;
    case 0xD2: mode = M_STATUS; return;
    case 0xE1: mode = M_STATUS, status_lo = 0x01; return;
    case 0xF0: mode = M_READ; return;
    default: os_die("flash: unknown command", v);
    }
}

void flash_dma_read(uint32_t dram, uint32_t off, uint32_t len) {
    if (dram + len > RDRAM_SIZE) os_die("flash: DMA past the end of RDRAM", dram);
    uint32_t at = halved ? off * 2 : off, st[2] = { status_hi(), id_maker };
    for (uint32_t i = 0; i < len; i++) {
        uint8_t b = mode == M_READ ? (at + i < FLASH_BYTES ? img[at + i] : 0xFF)
                                   : (uint8_t)(st[(i >> 2) & 1] >> (24 - 8 * (i & 3)));
        M8(0x80000000u | (dram + i)) = b;
    }
}

void flash_dma_write(uint32_t dram, uint32_t off, uint32_t len) {
    (void)off;
    if (dram + len > RDRAM_SIZE) os_die("flash: DMA past the end of RDRAM", dram);
    if (mode != M_WRITE) return;
    for (uint32_t i = 0; i < len && i < FLASH_PAGE; i++) buf[i] = M8(0x80000000u | (dram + i));
}
