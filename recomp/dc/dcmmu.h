#ifndef DCMMU_H
#define DCMMU_H

#include <stdint.h>

// The guest's RDRAM image at rdram (1 MB-aligned): the biased view, the boot stack, KOS's basic MMU (store queues in
// UTLB 62-63) and the rest for hostmmu, and TLB faults reported in guest addresses
void dcmmu_init(void *rdram);
extern void (*dcmmu_on_fault)(uint32_t pc, uint32_t pr);  // after the fault line, before the abort

#endif
