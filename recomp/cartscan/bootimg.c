// The power-on RDRAM image: IPL3's 1 MB copy and stage 2, entry.s's bss clear, IPL3's globals at 0x300
#include "boot.h"

#include <string.h>

#define IPL3_COPY 0x100000u
#define OP_LUI 0x0F
#define OP_ADDIU 0x09
#define OP_ORI 0x0D

static uint32_t rom_w(cs_page_fn page, void *ctx, uint32_t idx) { return page(ctx, idx >> 10)[idx & 1023]; }

static void put_byte(uint32_t *img, uint32_t a, uint8_t b) {
    uint32_t sh = 8 * (3 - (a & 3));
    img[a >> 2] = (img[a >> 2] & ~(0xFFu << sh)) | (uint32_t)b << sh;
}

// IPL3 copies its stage 2 (DMEM [t0, t3), set by an addiu pair) to RDRAM dst; it is still there when the game runs
static void ipl3_stage2(uint32_t *img, const uint32_t *page0, uint32_t seed, uint32_t dst) {
    for (uint32_t i = 0x10; i < 0x3FF; i++) {
        if (page0[i] >> 16 != 0x2508 || page0[i + 1] >> 16 != 0x256B) continue;
        uint32_t lo = page0[i] & 0xFFC, hi = page0[i + 1] & 0xFFC;
        if (lo < 0x40 || hi <= lo || hi - lo > 0x340) return;
        uint32_t key = seed ? seed * 0x0260BCD5u + 1 : 0;  // 6106 decrypts it word by word with this LCG
        for (uint32_t a = lo; a < hi; a += 4, key *= 0x0260BCD5u) img[(dst + a - lo) / 4] = page0[a / 4] ^ key;
        return;
    }
}

const char *bi_build(uint32_t *img, cs_page_fn page, void *ctx, uint32_t rom_size, BootInfo *bi) {
    memset(bi, 0, sizeof *bi);
    if (rom_size < 0x1000 + 32 * 4) return "ROM too small for entry.s";
    for (int i = 0; i < 16; i++) bi->header[i] = rom_w(page, ctx, i);
    const CsIpl3 *ipl;
    bi->entry = cs_boot_load(page(ctx, 0), &ipl);
    bi->sp_fill = ipl->sp_fill, bi->imem0 = ipl->imem0;
    uint32_t regs[32], have = 0;
    int jr = -1, done = 0;
    for (int i = 0; i < 32 && !done; i++) {
        uint32_t w = rom_w(page, ctx, 0x1000 / 4 + i), op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31;
        if (op == OP_LUI) regs[rt] = (w & 0xFFFF) << 16, have |= 1u << rt;
        else if (op == OP_ADDIU && rs == 0) regs[rt] = (uint32_t)(int32_t)(int16_t)w, have |= 1u << rt;  // li: DK64's bss size
        else if (op == OP_ORI && rs == 0) regs[rt] = w & 0xFFFF, have |= 1u << rt;
        else if (op == OP_ADDIU && rs == rt && (have >> rt & 1)) regs[rt] += (uint32_t)(int32_t)(int16_t)w;
        else if (op == OP_ORI && rs == rt && (have >> rt & 1)) regs[rt] |= w & 0xFFFF;
        if (jr >= 0) done = 1;
        else if (op == 0 && (w & 63) == 8) jr = rs;
    }
    if (!done) return "entry: no jr in the first instructions";
    if (!(have >> jr & 1) || !(have >> 8 & 1) || !(have >> 9 & 1) || !(have >> 29 & 1))
        return "entry: main, bss or sp not built from constants";
    bi->main = regs[jr], bi->bss = regs[8], bi->bss_size = regs[9], bi->sp = regs[29];
    memset(img, 0, BOOT_RDRAM);
    ipl3_stage2(img, page(ctx, 0), ipl->seed, ipl->os_cic_id == 6105 ? 4 : 0);
    uint32_t off = bi->entry - 0x80000000u, n = IPL3_COPY;
    if (rom_size - 0x1000 < n) n = rom_size - 0x1000;
    if (off >= BOOT_RDRAM) return "entry outside RDRAM";
    if (off + n > BOOT_RDRAM) n = BOOT_RDRAM - off;
    if (!(off & 3)) {
        for (uint32_t i = 0; i < n / 4; i++) img[off / 4 + i] = rom_w(page, ctx, 0x1000 / 4 + i);
        for (uint32_t i = n & ~3u; i < n; i++)
            put_byte(img, off + i, (uint8_t)(rom_w(page, ctx, (0x1000 + i) >> 2) >> (8 * (3 - (i & 3)))));
    } else {
        for (uint32_t i = 0; i < n; i++)
            put_byte(img, off + i, (uint8_t)(rom_w(page, ctx, (0x1000 + i) >> 2) >> (8 * (3 - (i & 3)))));
    }
    uint32_t lo = bi->bss - 0x80000000u, end = lo + bi->bss_size;
    if (lo < BOOT_RDRAM) {
        if (end > BOOT_RDRAM || end < lo) end = BOOT_RDRAM;
        for (uint32_t a = lo; a < end; a++) {
            if (!(a & 3) && a + 4 <= end) {
                img[a >> 2] = 0;
                a += 3;
            } else {
                put_byte(img, a, 0);
            }
        }
    }
    const uint32_t ipl3[7] = {1, 0, 0xB0000000u, 0, ipl->os_cic_id, 0, BOOT_RDRAM};  // NTSC, cart, rom base, cold, osCicId, v0, 4 MB
    memcpy(img + 0x300 / 4, ipl3, sizeof ipl3);
    if (ipl->os_cic_id == 6105) {
        img[0x3F0 / 4] = BOOT_RDRAM;  // stage 1's RDRAM size, which stage 2 copies to osMemSize
        // its RSP copies IMEM 0x120-0x1E0 to RDRAM in 8-byte rows from 0x2FB1F0, 0xFF0 apart; only IMEM 0x138 is known
        img[0x2FE1C0 / 4] = 0xAD170014u;
    }
    return NULL;
}
