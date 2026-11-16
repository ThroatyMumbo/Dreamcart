#ifndef DC64_CART_H
#define DC64_CART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Dreamcart 64 menu's cart: what the reader says about it, and the reader jobs that run off the UI thread
enum dc64_state { ST_BUSY, ST_NOREADER, ST_READY, ST_NOCART, ST_OFF, ST_PAK, ST_GO };
enum dc64_job { J_PROBE = 1, J_OFF, J_ON, J_PAK };

struct dc64_cart {
    char title[32], code[5];
    const char *region;
    unsigned version;
    uint32_t rom_mb, save, cache;
};
extern struct dc64_cart dc64_cart;  // the last job that found a cart filled it; read it once that job has ended

bool dc64_cart_save_ok(void);  // a save the play build serves: none, or a 4 or 16 Kbit EEPROM
int dc64_pak_free(void);       // the VMU's free blocks at the last J_PAK, else -1

void dc64_job_start(enum dc64_job job);
bool dc64_job_running(void);
// Once the job has ended: the state it left, its note (if it made one) copied into note; else -1
int dc64_job_end(char *note, size_t cap);

#endif
