#ifndef RSP_TASK_H
#define RSP_TASK_H

#include "htypes.h"

// An RSP task that is neither graphics nor audio (OSTask at DMEM 0xFC0), picked by its microcode's checksum as
// mupen64plus-rsp-hle does; returns the work it did in macroblocks, for the task's modeled length.
uint32_t rsp_other_task(uint8_t *dmem, uint32_t type);

#endif
