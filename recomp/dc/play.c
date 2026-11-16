// The realtime build: power-on boot of the real cart, Count on the console clock, frames through the PVR sink
#include <kos.h>
#include <malloc.h>
#include <string.h>
#include "play.h"
#include "dc_exit.h"
#include "view.h"
#include "os.h"
#include "rcp.h"
#include "pif.h"
#include "cartlink.h"
#include "dcmmu.h"
#include "rsp_gfx.h"
#include "gfx.h"
#include "gfx_sink.h"
#include "gfx_tex.h"
#include "gfx_pvr.h"
#include "romcache.h"
#include "cartboot.h"
#include "vmupak.h"
#include "cartflash.h"
#include "flashram.h"
#include "tmpl.h"
#include "drivers.h"
#ifdef DC64_MENU
#include "dc64_menu.h"
#include "dc64_restart.h"
#endif

#define OWN_STACK 0x10000u
#define HEAP_SPARE 0x80000u   // left to KOS after the ROM cache takes the rest of the heap
#define ROMCACHE_GIFT 0x40000u  // the ROM cache's heap comes in these, each one it can give back
#define DCACHE_SPAN 0x4000u   // the SH-4's operand cache: direct-mapped, 16 KB, sets from address bits 13:5
#define RSP_HOT 0x800u        // the RSP fiber's stack depth in use; Kirby 64 peaks at 1656 bytes
#define TRIG_HELD 0x40        // an analog trigger counts as pressed past a quarter
#define FPSCR_DN 0x00040000u  // single precision, round to nearest, denormals flushed to zero

uint8_t *play_rdram;

static uint8_t *pool;  // the n64 thread's stacks, from the heap after the boot: a first boot's scan needs that room
static uint32_t pool_os_bytes;  // the boot's big stack and the OS's pool; the rest of the pool is run()'s own
static uint64_t clock_t0;
static void (*combo_next)(void), (*deadline_next)(void);

void play_load_failed(void) {
#ifdef DC64_MENU
    if (!dc64_restart_ready()) {
        printf("dc64: .data outgrew the restart snapshot, so the failed load exits to dcload\n");
        return;
    }
    dc64_restart_note("Failed to load");
    printf("dc64: the cart didn't load, back to the menu\n");
    dc64_restart_arm();
#endif
}

void play_abort(const char *why) {
    printf("play: abort, %s\n", why);
    play_load_failed();
    fflush(stdout);
    arch_abort();
}

#ifdef DC64_MENU
static void back_to_menu(void) {
    if (!dc64_restart_ready()) {
        printf("dc64: .data outgrew the restart snapshot, so the combo exits to dcload\n");
        return;
    }
    printf("dc64: back to the menu\n");
    dc64_restart_arm();
}
#endif

// The pool's offset in raw that puts the RSP fiber's hot stack in no D-cache set of the vertex buffer or its batches:
// gfx lighting alternates between its stack and the batch's lights, and a shared set misses on every channel
static uint32_t rsp_stack_offset(const uint8_t *raw) {
    static uint8_t used[DCACHE_SPAN / 32];
    const struct { uintptr_t a; uint32_t n; } hot[2] = { { (uintptr_t)gfx_batches, gfx_batches_bytes },
                                                         { (uintptr_t)g.v, sizeof g.v } };
    for (int i = 0; i < 2; i++)
        for (uintptr_t x = hot[i].a & ~31u; x < hot[i].a + hot[i].n; x += 32) used[(x % DCACHE_SPAN) / 32] = 1;
    for (uint32_t off = 0; off < DCACHE_SPAN; off += 32) {
        uintptr_t top = (uintptr_t)raw + off + pool_os_bytes;
        int clear = 1;
        for (uintptr_t x = top - RSP_HOT; x < top && clear; x += 32) clear = !used[(x % DCACHE_SPAN) / 32];
        if (clear) return off;
    }
    return 0;
}

static unsigned long long clock_counts(void) {
    return (timer_ns_gettime64() - clock_t0) * 3ull / 64ull;  // N64 Count runs at 46.875 MHz
}

static void clock_wait(unsigned long long until) {
    uint64_t t0 = timer_us_gettime64(), rep0 = stats_report_us();
    while (clock_counts() < until) {
        stats_report();
        if (!gfx_pvr_idle()) thd_pass();
    }
    stats_idle(timer_us_gettime64() - t0 - (stats_report_us() - rep0));
}

static int8_t stick(uint32_t raw) {
    int v = ((int)(raw & 0xFF) - 128) * 80 / 128;
    return (int8_t)(v > -8 && v < 8 ? 0 : v);
}

// D-pad is the N64 D-pad, or the C buttons while the R trigger is held; L trigger and X are Z, Y is R
static void live_pad(void) {
    static const uint16_t map[][3] = {  // DC button, N64 bit, N64 bit with the R trigger held
        { CONT_A, 0x8000, 0x8000 }, { CONT_B, 0x4000, 0x4000 }, { CONT_X, 0x2000, 0x2000 },
        { CONT_START, 0x1000, 0x1000 }, { CONT_Y, 0x0010, 0x0010 },
        { CONT_DPAD_UP, 0x0800, 0x0008 }, { CONT_DPAD_DOWN, 0x0400, 0x0004 },
        { CONT_DPAD_LEFT, 0x0200, 0x0002 }, { CONT_DPAD_RIGHT, 0x0100, 0x0001 },
    };
    uint32_t p = dc_exit_pad(), trig = dc_exit_triggers(), shift = (trig & 0xFF) >= TRIG_HELD;
    uint16_t b = (trig >> 8) >= TRIG_HELD ? 0x2000 : 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (p & map[i][0]) b |= map[i][1 + shift];
    pif_set_pad(b, stick(p >> 16), (int8_t)-stick(p >> 24));
}

// A framebuffer the CPU drew (Rayman 2's Controller Pak screens): shown once graphics tasks have stopped, unless the
// VI points into one the RSP drew (origin is a line past it in LAN1 modes); refreshed every 4 fields
static uint32_t vi_field, gfx_field, cpu_origin, cpu_field;
static int gfx_seen;

static void on_vi(uint32_t field) {
    live_pad();  // every field, so a game behind real time still sees the pad
    vi_field = field;
    if (gfx_seen && field - gfx_field < 30) return;
    uint32_t ctrl = rcp_vi_reg(0), origin = rcp_vi_reg(1) & 0xFFFFFF, width = rcp_vi_reg(2) & 0xFFF;
    uint32_t vs = rcp_vi_reg(10), ys = rcp_vi_reg(13) & 0xFFF, lines = ((vs & 0x3FF) - (vs >> 16 & 0x3FF)) / 2;
    if ((ctrl & 3) != 2 || !origin || !(rcp_vi_reg(9) & 0x3FF03FF) || gfx_pvr_drew(origin)) return;
    if (origin == cpu_origin && field - cpu_field < 4) return;
    uint32_t h = ys ? (lines * ys + 0x200) >> 10 : width * 3 / 4;
    if (gfx_pvr_show_rdram(origin, (int)width, (int)(h > 240 ? 240 : h))) cpu_origin = origin, cpu_field = field;
}

static void on_task_start(uint32_t type) {
    (void)type;
    stats_task_start();
}

static void on_task_pause(uint32_t type) {
    (void)type;
    stats_task_pause();
}

static void on_gfx(const uint8_t *spmem, uint32_t pc, const struct gfx_stats *st, unsigned long long done_at) {
    (void)spmem, (void)pc, (void)done_at;
    gfx_seen = 1, gfx_field = vi_field;
    stats_gfx_done(st->tris);
}

static void on_audio(uint32_t list, uint32_t cmds, unsigned long long done_at) {
    (void)list, (void)cmds, (void)done_at;
    stats_audio_done();
}

static void on_pi(uint32_t off, uint32_t dram, uint32_t len, unsigned long long at) {
    (void)off, (void)dram, (void)at;
    stats_pi(len);
}

static void play_attach(void) {
    rcp_hooks.task_start = on_task_start;
    rcp_hooks.task_pause = on_task_pause;
    rcp_hooks.gfx_task = on_gfx;
    rcp_hooks.audio_task = on_audio;
    rcp_hooks.pi = on_pi;
    rcp_hooks.vi = on_vi;
}

static void save_block(uint32_t block, const uint8_t data[8]) {
    uint32_t b[2];
    memcpy(b, data, 8);
    if (cart_save_write(block * 8, b, 8)) printf("play: cart save block %lu written\n", (unsigned long)block);
    else printf("play: cart save block %lu write FAILED\n", (unsigned long)block);
}

// The game's EEPROM is the cart's: loaded from the chip at boot, and every block the game writes goes back to it
static void load_cart_save(void) {
    static uint32_t save[512];
    uint32_t size = cart_save_size();
    if (!size) {  // no chip, or one whose boot reads disagreed (the reader's console says which)
        printf("play: the reader serves no cart save; the game sees no EEPROM\n");
        pif_no_eeprom();
        return;
    }
    if (size == FLASH_BYTES) {
        pif_no_eeprom();
        if (!cartflash_open()) play_abort("the cart's FlashRAM didn't load");
        return;
    }
    if (size != 512 && size != sizeof save) {
        printf("play: the cart's save chip is %lu bytes; the PIF models 4 and 16 Kbit EEPROMs\n", (unsigned long)size);
        play_abort("unsupported save chip");
    }
    if (!cart_save_read(0, save, size)) play_abort("the cart's save didn't read back cleanly");
    printf("play: cart save %lu bytes, CRC32 %08lX\n", (unsigned long)size, (unsigned long)cart_crc32(save, size));
    pif_load_eeprom((const uint8_t *)save, size);
    pif_on_eeprom_write = save_block;
}

// The pak's last writes reach the VMU before an exit or a restart into the menu
static void exit_combo(void) {
    boot_hang_report();  // a freeze the combo ends says where it was
    vmupak_flush();
    cartflash_flush();
    if (combo_next) combo_next();
}

static void exit_deadline(void) {
    vmupak_flush();
    cartflash_flush();
    if (deadline_next) deadline_next();
}

static int cart_fetch(uint32_t off, void *dst, uint32_t len) { return cart_read(off, dst, len); }

static int cart_rom(uint32_t off, void *dst, uint32_t len) {
    boot_rom_poll();
    uint64_t t0 = timer_us_gettime64();
    int ok = romcache_read(off, dst, len);
    stats_pi_wait(timer_us_gettime64() - t0);
    return ok;
}

// The largest block malloc can hand out: the top chunk plus what sbrk has left below KOS's kernel stack
static uint32_t heap_largest(void) {
    uint32_t brk = (uint32_t)mm_sbrk(0), lim = _arch_mem_top - THD_KERNEL_STACK_SIZE;
    return mallinfo().keepcost + (lim > brk ? lim - brk : 0);
}

uint32_t play_heap_to_romcache(uint32_t keep) {
    uint32_t heap = 0, spare = HEAP_SPARE + keep, top = heap_largest();
    top = top >= 0x810000u ? 0x800000u : top >= 0x20000u ? (top & ~0xFFFFu) - 0x10000u : 0;  // each failed try prints a line
    for (uint32_t n = top; n > spare; n -= 0x10000u) {
        void *p = malloc(n);
        if (!p) continue;
        free(p);
        heap = n - spare;
        break;
    }
    uint32_t given = 0;
    while (heap - given >= 2 * 4096u) {  // in pieces the cache gives back when run-time code needs the heap
        uint32_t n = heap - given < ROMCACHE_GIFT ? heap - given : ROMCACHE_GIFT;
        void *p = malloc(n);
        if (!p) break;
        romcache_add_heap(p, n);
        given += n;
    }
    cartboot_reclaim = romcache_release;
    return given;
}

static void *run(void *arg) {
    (void)arg;
    __asm__ volatile("lds %0,fpscr" : : "r"(FPSCR_DN));
    os_set_stack_pool(pool, pool_os_bytes);
    rcp_set_cart(cart_rom, 0);
    rcp_set_cart_peek(romcache_read);  // the audio cache's frame checks and overlay reads aren't PI time
    load_cart_save();
    vmupak_open();
    gfx_pvr_init();
    gfx_tex_reset();
    romcache_init(cart_fetch);
    boot_rom_cache();
    gfx_sink = &gfx_sink_pvr;
    clock_t0 = timer_ns_gettime64();
    stats_start();
    os_set_clock(clock_counts, clock_wait);
    play_attach();
    os_driver_main();
    return 0;
}

int main(int argc, char **argv) {
    (void)argc, (void)argv;
    if (!dc_exit_arm()) return 1;
#ifdef PLAY_QUIET
    printf("play: quiet\n");
    dbgio_dev_select("null");
#endif
    play_rdram = memalign(0x100000, RDRAM_BYTES);
    if (!play_rdram) {  // no menu restart: it would only come back here
        printf("play: abort, no memory for the RDRAM image\n");
        fflush(stdout);
        arch_abort();
    }
    printf("play: boot image at %p\n", play_rdram);
#ifdef DC64_MENU
    dc64_menu();
    dc_exit_on_combo = back_to_menu;
#endif
    boot_load(play_rdram);
    dcmmu_init(play_rdram);
    pool_os_bytes = boot_stack_bytes() + OS_STACK_POOL_BYTES;
    uint32_t pool_bytes = pool_os_bytes + OWN_STACK;
    uint8_t *raw = memalign(32, pool_bytes + DCACHE_SPAN);
    if (!raw) play_abort("no memory for the n64 thread's stacks");
    pool = raw + rsp_stack_offset(raw);
    printf("play: n64 stacks at %p, RSP stack top %p\n", pool, pool + pool_os_bytes);
    kthread_attr_t attr = { .stack_size = pool_bytes, .stack_ptr = pool, .label = "n64" };
    kthread_t *t = thd_create_ex(&attr, run, 0);
    if (!t) play_abort("no n64 thread");
    boot_watch(t);
    combo_next = dc_exit_on_combo, deadline_next = dc_exit_on_deadline;
    dc_exit_on_combo = exit_combo, dc_exit_on_deadline = exit_deadline;
    thd_join(t, 0);
    vmupak_flush();
    printf("play: done\n");
    fflush(stdout);
    arch_abort();
    return 0;
}
