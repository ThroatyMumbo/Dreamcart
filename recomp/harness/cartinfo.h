#ifndef CARTINFO_H
#define CARTINFO_H

#include <stdint.h>

// What the OS HLE and RCP need from a particular ROM, filled in by cartboot's scan of it
struct cart_info {
    uint32_t thread_tail, event_tab, rcp_im_table, destroy_thread;  // libultra's __osThreadTail etc.
    uint32_t main, sp;                                              // entry.s's jump to main, and its stack
    uint32_t header[16];                                            // ROM 0x00-0x3F, for PI reads
    uint32_t sp_fill, imem0;                                        // SP memory as IPL3 leaves it
};

extern struct cart_info cart_info;

#endif
