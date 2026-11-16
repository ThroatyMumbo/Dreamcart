// The cart's FlashRAM, as the game sees it: the whole chip in RAM, loaded through the reader; a background thread
// sends each page the game erased or programmed back to the cart once its writes settle.
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arch/irq.h>
#include <arch/timer.h>
#include <kos/mutex.h>
#include <kos/sem.h>
#include <kos/thread.h>
#include "cartlink.h"
#include "flashram.h"
#include "cartflash.h"

#define PAGES (FLASH_BYTES / FLASH_PAGE)
#define NW (PAGES / 32)
#define SETTLE_MS 200   // an erase and its program land on one page in a few ms; a save writes many pages
#define RETRY_MS 2000

static uint8_t *img;
static volatile uint32_t dirty[NW];
static volatile uint64_t last_write;
static mutex_t lock = MUTEX_INITIALIZER;
static semaphore_t wake = SEM_INITIALIZER(0);
static volatile int woken;

static int any_dirty(void) {
    for (unsigned i = 0; i < NW; i++)
        if (dirty[i]) return 1;
    return 0;
}

static void mark(uint32_t page) {
    irq_mask_t s = irq_disable();
    dirty[page >> 5] |= 1u << (page & 31);
    irq_restore(s);
    last_write = timer_ms_gettime64();
    if (!woken) woken = 1, sem_signal(&wake);
}

// Every dirty page; 0 if a write failed, with it and the rest still dirty
static int write_back(unsigned *written) {
    static uint32_t page[FLASH_PAGE / 4];
    *written = 0;
    for (uint32_t p = 0; p < PAGES; p++) {
        uint32_t bit = 1u << (p & 31);
        if (!(dirty[p >> 5] & bit)) continue;
        irq_mask_t s = irq_disable();
        dirty[p >> 5] &= ~bit;  // cleared before the copy: a write after it marks the page again
        irq_restore(s);
        memcpy(page, img + p * FLASH_PAGE, FLASH_PAGE);
        if (!cart_save_write(p * FLASH_PAGE, page, FLASH_PAGE)) {
            mark(p);
            printf("flash: page %lu write FAILED\n", (unsigned long)p);
            return 0;
        }
        (*written)++;
    }
    return 1;
}

static int flush_locked(void) {
    unsigned n;
    uint64_t t0 = timer_ms_gettime64();
    int ok = write_back(&n);
    if (n) printf("flash: %u pages to the cart in %lu ms\n", n, (unsigned long)(timer_ms_gettime64() - t0));
    return ok;
}

static void *writer(void *arg) {
    (void)arg;
    for (;;) {
        sem_wait(&wake);
        for (;;) {
            uint64_t quiet = timer_ms_gettime64() - last_write;
            if (quiet < SETTLE_MS) {
                thd_sleep((int)(SETTLE_MS - quiet));
                continue;
            }
            woken = 0;
            mutex_lock(&lock);
            int ok = flush_locked();
            mutex_unlock(&lock);
            if (ok) break;
            thd_sleep(RETRY_MS);
        }
    }
    return NULL;
}

int cartflash_open(void) {
    uint32_t w[N64_MI_END];
    if (!cart_media_info(w, N64_MI_END) || w[N64_MI_SAVE_ID0] != 0x11118001u) {
        printf("flash: the reader gives no FlashRAM id\n");
        return 0;
    }
    img = memalign(32, FLASH_BYTES);
    if (!img) {
        printf("flash: no memory for the chip\n");
        return 0;
    }
    uint64_t t0 = timer_ms_gettime64();
    if (!cart_save_read(0, img, FLASH_BYTES)) {
        printf("flash: the cart's FlashRAM didn't read back cleanly\n");
        free(img);
        img = NULL;
        return 0;
    }
    printf("flash: %08lX %08lX, 128 KB in %lu ms, CRC32 %08lX\n", (unsigned long)w[N64_MI_SAVE_ID0],
           (unsigned long)w[N64_MI_SAVE_ID1], (unsigned long)(timer_ms_gettime64() - t0),
           (unsigned long)cart_crc32(img, FLASH_BYTES));
    flash_on_page = mark;
    flash_load(img, w[N64_MI_SAVE_ID0], w[N64_MI_SAVE_ID1]);
    kthread_t *t = thd_create(true, writer, NULL);
    if (t) thd_set_prio(t, PRIO_DEFAULT + 1);  // below the game's thread
    else printf("flash: no writer thread\n");
    return 1;
}

void cartflash_flush(void) {
    if (!img || !any_dirty()) return;
    mutex_lock(&lock);
    flush_locked();
    mutex_unlock(&lock);
}
