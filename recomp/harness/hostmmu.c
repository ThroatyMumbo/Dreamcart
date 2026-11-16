#include "hostmmu.h"
#include "os.h"

#define UTLB_N 64
#define MMUCR (*(volatile uint32_t *)0xFF000010u)
#define MMUCR_AT 0x001u
#define UTLB_ADDR(i) (*(volatile uint32_t *)(0xF6000000u | (uint32_t)(i) << 8))
#define UTLB_DATA(i) (*(volatile uint32_t *)(0xF7000000u | (uint32_t)(i) << 8))
#define A_V 0x100u
#define A_D 0x200u
#define D_V 0x100u
#define D_PR_RW 0x020u
#define D_C 0x008u
#define D_D 0x004u
#define D_SH 0x002u

static const uint32_t sz_bytes[4] = { 0x400u, 0x1000u, 0x10000u, 0x100000u };
static const uint32_t sz_bits[4] = { 0x00u, 0x10u, 0x80u, 0x90u };

static uint32_t want_addr[UTLB_N], want_data[UTLB_N];
static int n_used, n_written, at_on, top = UTLB_N, at_always;

static int translated(uint32_t va) { return va < 0x80000000u || va >> 29 == 6; }

// The UTLB arrays and MMUCR are only written from P2 on the SH-4; it touches nothing but these tables
static void __attribute__((noinline)) utlb_write(void) {
    for (int i = 0; i < n_written; i++) {
        UTLB_DATA(i) = want_data[i];
        UTLB_ADDR(i) = want_addr[i];
    }
    MMUCR = (MMUCR & ~(MMUCR_AT | 4u)) | (at_on ? MMUCR_AT : 0);
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop" ::: "memory");
}

static void commit(void) {
    ((void (*)(void))(((uint32_t)utlb_write & 0x1FFFFFFFu) | 0xA0000000u))();
}

static void put(int i, uint32_t va, uint32_t pa, int s, int writable, int cached) {
    want_addr[i] = (va & 0xFFFFFC00u) | A_D | A_V;
    want_data[i] = (pa & 0x1FFFFC00u) | D_V | sz_bits[s] | (writable ? D_PR_RW : 0) | (cached ? D_C : 0) | D_D | D_SH;
}

void hostmmu_reserve_top(int n) {
    top = UTLB_N - n;
    at_always = 1;
    for (int i = 0; i < top; i++) want_addr[i] = want_data[i] = 0;
    n_written = top;  // whatever the boot left in our slots is invalidated before AT can reach it
    hostmmu_end();
}

void hostmmu_begin(void) { n_used = 0; }

void hostmmu_page(uint32_t va, uint32_t pa, uint32_t bytes, int writable, int cached) {
    if ((va | pa | bytes) & 0x3FFu) os_die("hostmmu: page not 1 KB aligned", va);
    while (bytes) {
        int s = 3;
        while (s && (sz_bytes[s] > bytes || ((va | pa) & (sz_bytes[s] - 1)))) s--;
        if (!translated(va)) os_die("hostmmu: guest page lands outside the host's translated areas", va);
        if (n_used == top) os_die("hostmmu: more guest pages than free UTLB entries", va);
        // The operand cache is indexed by VA bits 13:5; a page whose VA and PA disagree there would alias, so leave it uncached
        put(n_used++, va, pa, s, writable, cached && !((va ^ pa) & 0x3000u));
        va += sz_bytes[s];
        pa += sz_bytes[s];
        bytes -= sz_bytes[s];
    }
}

void hostmmu_end(void) {
    for (int i = n_used; i < n_written; i++) want_addr[i] = want_data[i] = 0;
    if (n_written < n_used) n_written = n_used;
    at_on = at_always || n_used > 0;
    commit();
    n_written = n_used;
}

