// RSP audio tasks through mupen64plus-rsp-hle's ABI1, nead and naudio command list code (recomp/mupen64plus-rsp-hle, unmodified).
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "hle_internal.h"
#include "ucodes.h"

#include "alist_aica.h"
#include "alist_decode.h"

#include "os.h"
#include "rsp_audio.h"
#include "sim.h"
#include "tmpl.h"

#define ABI1_KEY 0x1E24138Cu  // ucode_data + 0x28 for the common audio ABI (mupen's hle.c)
#define NEAD_MK_KEY 0x11181350u  // ucode_data + 0x10: MarioKart, WaveRace (E) (mupen's hle.c)
#define NEAD_MM_KEY 0x1F801250u  // ucode_data + 0x10: Zelda MM, Pokemon Stadium 2 (mupen's hle.c)
#define NAUDIO_KEY 0x127Cu       // ucode_data + 0x10 when its first word isn't 1: naudio (mupen's hle.c)
#define NAUDIO_BK_KEY 0x1280u    // Banjo-Kazooie's naudio, the same command table

static struct hle_t hle;
uint32_t rsp_audio_break;

void rsp_break(struct hle_t *h, unsigned int setbits) { (void)h; rsp_audio_break |= setbits; }
void HleVerboseMessage(void *u, const char *m, ...) { (void)u; (void)m; }
void HleInfoMessage(void *u, const char *m, ...) { (void)u; (void)m; }
void HleWarnMessage(void *u, const char *m, ...) {
    (void)u;
    if (!strcmp(m, "Invalid segment %u")) {  // mupen goes on with the bare 24-bit offset, which is what the DMA sees
        static int told;
        va_list ap;
        va_start(ap, m);
        unsigned seg = va_arg(ap, unsigned);
        va_end(ap);
        if (!told++) sim_puts("rsp audio: address past the segment table, using its offset; "), sim_hex("segment", seg), sim_puts("\n");
        return;
    }
    os_die(m, 0);
}
void HleErrorMessage(void *u, const char *m, ...) { (void)u; os_die(m, 0); }
void HleCheckInterrupts(void *u) { (void)u; }

static uint32_t rd(uint32_t phys) { return *(volatile uint32_t *)(0x80000000u + phys + t_rdram_bias); }

uint32_t rsp_audio_task(uint8_t *dmem, uint32_t hz) {
    uint32_t data = *(uint32_t *)(dmem + 0xFD8) & 0xFFFFFF;
    if (data > RDRAM_BYTES - 0x40) os_die("rsp audio: command addresses RDRAM out of range", 0x81u << 24 | data);
    int abi = -1;
    if (rd(data) == 1 && rd(data + 0x30) == 0xF0000F00u && rd(data + 0x28) == ABI1_KEY) abi = AA_ABI1;
    else if (rd(data) == 1 && rd(data + 0x30) != 0xF0000F00u && rd(data + 0x10) == NEAD_MK_KEY) abi = AA_NEAD;
    else if (rd(data) == 1 && rd(data + 0x30) != 0xF0000F00u && rd(data + 0x10) == NEAD_MM_KEY) abi = AA_NEAD_MM;
    else if (rd(data) != 1 && (rd(data + 0x10) == NAUDIO_KEY || rd(data + 0x10) == NAUDIO_BK_KEY)) abi = AA_NAUDIO;
    if (abi < 0) {
        sim_hex("rsp audio: ucode data", data);  // the words mupen's try_audio_task_detection keys on
        sim_hex(" +0", rd(data)), sim_hex(" +10", rd(data + 0x10));
        sim_hex(" +28", rd(data + 0x28)), sim_hex(" +30", rd(data + 0x30));
        sim_puts("\n");
        os_die("rsp audio: microcode is not the common audio ABI, a known nead or naudio", rd(data + 0x10));
    }
    static int said = -1;
    if (abi != said) {
        sim_hex("rsp audio: abi", (uint32_t)abi), sim_hex(" ucode data", data);
        sim_puts("\n");
        said = abi;
    }
    hle.dram = (unsigned char *)(0x80000000u + t_rdram_bias);
    hle.dmem = dmem;
    hle.imem = dmem + 0x1000;
    uint32_t list = *(uint32_t *)(dmem + 0xFF0) & 0xFFFFFF, size = *(uint32_t *)(dmem + 0xFF4);
    uint32_t n = alist_decode(&hle, abi, list, size);
    rsp_audio_break = 0;
    audio_run(&hle, abi, hz);
    if (rsp_audio_break != SP_STATUS_TASKDONE) os_die("rsp audio: task ended without TASKDONE", rsp_audio_break);
    return n;
}
