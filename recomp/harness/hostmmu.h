#ifndef HOSTMMU_H
#define HOSTMMU_H

#include "htypes.h"

// The top n UTLB slots belong to someone else (KOS's store-queue pair, 62-63) and AT stays on for them
void hostmmu_reserve_top(int n);

// Rebuild the guest's entries: begin, one page() per mapped range (host VA, host PA), end writes the UTLB
void hostmmu_begin(void);
void hostmmu_page(uint32_t va, uint32_t pa, uint32_t bytes, int writable, int cached);
void hostmmu_end(void);


#endif
