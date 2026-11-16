// RSP tasks besides graphics and audio, through mupen64plus-rsp-hle's own routines (unmodified), chosen by its
// try_normal_task_detection checksums: the JPEG decoders, and StoreVe12's task that has nothing to do.
#include <stdint.h>

#include "hle_internal.h"
#include "memory.h"
#include "ucodes.h"

#include "os.h"
#include "rsp_task.h"
#include "tmpl.h"

static struct hle_t hle;

static const uint8_t *ram(uint32_t phys) { return (const uint8_t *)(uintptr_t)(0x80000000u + phys + t_rdram_bias); }
static uint32_t rd(uint32_t phys) { return *(const uint32_t *)ram(phys); }

static void need(uint32_t a, uint32_t n, const char *why) {
    if (a >= RDRAM_BYTES || n > RDRAM_BYTES - a) os_die(why, a);
}

// mupen's sum_bytes over the ucode's first half, host bytes as its RDRAM holds them too
static uint32_t ucode_sum(const uint8_t *dmem) {
    uint32_t ucode = *(const uint32_t *)(dmem + TASK_UCODE) & 0xFFFFFF, size = *(const uint32_t *)(dmem + TASK_UCODE_SIZE);
    uint32_t n = (size < 0xF80 ? size : 0xF80) >> 1, sum = 0;
    need(ucode, n, "rsp task: microcode out of RDRAM");
    for (uint32_t i = 0; i < n; i++) sum += ram(ucode)[i];
    return sum;
}

// jpeg_decode_std's descriptor and the macroblocks it decodes in place; their count
static uint32_t jpeg_std_bounds(const uint8_t *dmem) {
    uint32_t d = *(const uint32_t *)(dmem + TASK_DATA_PTR) & 0xFFFFFF;
    need(d, 24, "rsp jpeg: descriptor out of RDRAM");
    uint32_t addr = rd(d) & 0xFFFFFF, n = rd(d + 4), mode = rd(d + 8);
    for (int q = 0; q < 3; q++) need(rd(d + 12 + 4 * q) & 0xFFFFFF, 128, "rsp jpeg: qtable out of RDRAM");
    if (n > RDRAM_BYTES / 512) os_die("rsp jpeg: macroblock count", n);
    need(addr, n * (mode + 4) * 128, "rsp jpeg: macroblocks out of RDRAM");
    return n;
}

uint32_t rsp_other_task(uint8_t *dmem, uint32_t type) {
    uint32_t sum = ucode_sum(dmem), n = 0;
    hle.dram = (unsigned char *)(uintptr_t)(0x80000000u + t_rdram_bias);
    hle.dmem = dmem;
    hle.imem = dmem + 0x1000;
    switch (sum) {
    case 0x278: break;  // StoreVe12 (Ocarina of Time): nothing to emulate
    case 0x2C85A: n = jpeg_std_bounds(dmem), jpeg_decode_PS0(&hle); break;  // Pokemon Stadium J
    case 0x2CAA6: n = jpeg_std_bounds(dmem), jpeg_decode_PS(&hle); break;   // Ocarina of Time, Pokemon Stadium 1 and 2
    case 0x130DE: case 0x278B0: {  // Ogre Battle, Bottom of the 9th
        uint32_t addr = *(const uint32_t *)(dmem + TASK_DATA_PTR) & 0xFFFFFF;
        n = *(const uint32_t *)(dmem + TASK_DATA_SIZE);
        if (n > RDRAM_BYTES / 768) os_die("rsp jpeg: macroblock count", n);
        need(addr, n * 768, "rsp jpeg: macroblocks out of RDRAM");
        jpeg_decode_OB(&hle);
        break;
    }
    default:
        os_die("rsp task: no routine for this microcode; its type and checksum", type << 24 | (sum & 0xFFFFFF));
    }
    return n;
}
