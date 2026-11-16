#ifndef RSP_AUDIO_H
#define RSP_AUDIO_H

#include "htypes.h"

// Runs the audio task whose OSTask is at DMEM 0xFC0 (word-swapped SP memory) for an AI at hz; returns its command count.
uint32_t rsp_audio_task(uint8_t *dmem, uint32_t hz);

// The list itself: audio_aica.c, on AICA voices
struct hle_t;
void audio_run(struct hle_t *h, int abi, uint32_t hz);
extern uint32_t rsp_audio_break;  // the SP status bits the list has set; the task must end on TASKDONE alone

#define ABI1_DMEM_BASE 0x5C0u  // alist_audio.c: every ABI1 DMEM operand is relative to it
// naudio's fixed DMEM layout and per-command byte count (mupen's alist_naudio.c)
#define NA_MAIN 0x4F0u
#define NA_MAIN2 0x660u
#define NA_DL 0x9D0u
#define NA_DR 0xB40u
#define NA_WL 0xCB0u
#define NA_WR 0xE20u
#define NA_COUNT 0x170u

#endif
