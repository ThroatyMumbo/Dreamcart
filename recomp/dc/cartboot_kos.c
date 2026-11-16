// KOS glue for the cart pipeline (cartscan/, xlate/, harness/cartboot.c): heap, clock, console
#include <kos.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cartboot.h"
#include "scan.h"
#include "xlate.h"

static void fail(const char *who, const char *msg) {
    printf("%s: abort, %s\n", who, msg);
    cartboot_aborting();
    fflush(stdout);
    arch_abort();
}

// The boot image is dead until cartboot's image stage writes it: what outgrows the heap before then goes there
static uint8_t *spill_lo, *spill_at, *spill_hi;
static uint32_t spill_live, spill_peak;

void cartboot_spill_open(void *base, size_t len) {
    spill_lo = spill_at = base, spill_hi = spill_lo + len;
    spill_live = spill_peak = 0;
}

void cartboot_spill_close(void) {
    if (!spill_lo) return;
    if (spill_live) {
        printf("cartboot: %lu blocks still in the boot image at its stage\n", (unsigned long)spill_live);
        fail("cartboot", "out of memory");
    }
    if (spill_peak) printf("cartboot: the heap overflowed %lu KB into the boot image\n", (unsigned long)(spill_peak >> 10));
    spill_lo = spill_at = spill_hi = NULL;
}

static int in_spill(const void *p) { return spill_lo && (const uint8_t *)p >= spill_lo && (const uint8_t *)p < spill_hi; }

static void *spill(size_t n) {
    size_t need = 8 + ((n + 7) & ~(size_t)7);
    if (!spill_lo || (size_t)(spill_hi - spill_at) < need) return NULL;
    uint32_t *h = (uint32_t *)spill_at;
    h[0] = (uint32_t)n;
    spill_at += need, spill_live++;
    if ((uint32_t)(spill_at - spill_lo) > spill_peak) spill_peak = (uint32_t)(spill_at - spill_lo);
    return h + 2;
}

uint32_t (*cartboot_reclaim)(void);

// After the boot the ROM cache holds the heap: run-time code translation takes it back a donation at a time
static void *heap_alloc(size_t n) {
    void *p = malloc(n);
    while (!p && cartboot_reclaim && cartboot_reclaim()) p = malloc(n);
    return p;
}

static void *any_alloc(size_t n) {
    void *p = heap_alloc(n);
    return p ? p : spill(n);
}

static void any_free(void *p) {
    if (!in_spill(p)) free(p);
    else if (!--spill_live) spill_at = spill_lo;
}

static void *any_realloc(void *p, size_t n) {
    if (!p) return any_alloc(n);
    size_t old;
    void *q;
    if (in_spill(p)) {
        old = ((uint32_t *)p)[-2];
        q = any_alloc(n);
    } else {
        if ((q = realloc(p, n))) return q;
        old = malloc_usable_size(p);
        q = any_alloc(n);
    }
    if (!q) return NULL;
    memcpy(q, p, old < n ? old : n);
    any_free(p);
    return q;
}

void *cs_alloc(size_t n) {
    void *p = any_alloc(n ? n : 1);
    if (!p) fail("cartboot", "out of memory");
    return p;
}
void *cs_realloc(void *p, size_t n) {
    p = any_realloc(p, n ? n : 1);
    if (!p) fail("cartboot", "out of memory");
    return p;
}
void cs_release(void *p) {
    if (p) any_free(p);
}
void cs_die(const char *msg) { fail("cartboot", msg); }
void cs_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void *xl_alloc(size_t n) {
    void *p = any_alloc(n ? n : 1);
    if (p) memset(p, 0, n);
    if (!p) fail("xlate", "out of memory");
    return p;
}
void *xl_realloc(void *p, size_t n) {
    p = any_realloc(p, n ? n : 1);
    if (!p) fail("xlate", "out of memory");
    return p;
}
void xl_free(void *p) {
    if (p) any_free(p);
}
void xl_die(const char *msg) { fail("xlate", msg); }
void xl_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
uint64_t xl_now_us(void) { return timer_us_gettime64(); }
uint32_t cartboot_heap_used(void) { return (uint32_t)mallinfo().uordblks; }
