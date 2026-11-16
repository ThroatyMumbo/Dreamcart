// The realtime build's boot from the inserted cart alone: scanned, translated and linked here, behind the loading screen
#include <kos.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include "play.h"
#include "cartboot.h"
#include "cartinfo.h"
#include "cartlink.h"
#include "cartsrc_reader.h"
#include "dc_exit.h"
#include "dcmmu.h"
#include "loading.h"
#include "os.h"
#include "ovl.h"
#include "rgn.h"
#include "romcache.h"
#include "vmulcd.h"


static CartSrc cb_src;
static CartBootStats cb_st;
static XlOut cb_out;
static volatile bool g_loading;
static volatile int cb_done;
static kthread_t *n64_thd;

uint32_t boot_stack_bytes(void) { return OS_BIG_STACK_BYTES; }  // overlays are translated on it

void cartboot_aborting(void) {
    if (g_loading) play_load_failed();
}

void cartboot_stage(int stage) {
    static int last = -1;
    static uint64_t t0;
    uint64_t now = timer_us_gettime64();
    uint32_t calls, bytes;
    uint64_t us;
    reader_take_reads(&calls, &bytes, &us);
    if (last >= 0)
        printf("cartboot: stage %d %lu ms, %lu ROM reads %lu KB, %lu ms of them on the link\n", last,
               (unsigned long)((now - t0) / 1000), (unsigned long)calls, (unsigned long)(bytes >> 10),
               (unsigned long)(us / 1000));
    last = stage, t0 = now;
    if (stage == CB_STAGE_IMAGE) cartboot_spill_close();
    loading_stage(stage);
}

static void cb_sync(void *p, size_t n) {
    dcache_flush_range((uintptr_t)p, n);
    icache_flush_range((uintptr_t)p, n);
}

static uint64_t cb_now(void) { return timer_us_gettime64(); }

static void *cb_thread(void *arg) {
    void *err = (void *)cartboot(&cb_src, (uint32_t *)arg, &cb_out, &cb_st, cb_sync, cb_now);
    cb_done = 1;
    return err;
}

static void commit_wait(void) {
    loading_stage(LOADING_COMMIT);
    int stored = reader_commit_wait(loading_draw);
    printf("cartboot: cache %s in the reader's flash\n", stored ? "stored" : "NOT stored");
}

static void print_stats(const CartBootStats *s) {
    printf("cartboot: %s; cache %lu ms, %s %lu ms, scan %lu ms, signatures %lu ms, meta and boot image %lu ms, "
           "xlate %lu ms%s, SD pack %lu ms, load %lu ms\n",
           s->cached ? "from the reader's cache" : "scanned", (unsigned long)(s->us_cache / 1000),
           s->tables ? "reader tables" : "stream", (unsigned long)(s->us_stream / 1000),
           (unsigned long)(s->us_scan / 1000), (unsigned long)(s->us_sigs / 1000), (unsigned long)(s->us_meta / 1000),
           (unsigned long)(s->us_xlate / 1000), s->xc_hit ? " (from the reader's SD)" : "",
           (unsigned long)(s->us_xc_put / 1000), (unsigned long)(s->us_load / 1000));
    printf("cartboot: %lu functions, %lu MIPS instructions, %lu hooked; meta %lu B crc %08lX, boot crc %08lX, "
           "text %lu B crc %08lX, %lu relocs crc %08lX, %lu stubs\n",
           (unsigned long)s->nfunc, (unsigned long)s->mips, (unsigned long)s->nhooked, (unsigned long)s->meta_len,
           (unsigned long)s->meta_crc, (unsigned long)s->boot_crc, (unsigned long)s->text, (unsigned long)s->text_crc,
           (unsigned long)s->nrel, (unsigned long)s->rel_crc, (unsigned long)s->stubs);
    printf("cartboot: main %08lX sp %08lX, code at %p\n", (unsigned long)cart_info.main, (unsigned long)cart_info.sp,
           cb_out.text);
}

// The header's title, trailing spaces gone, for the loading screen and the VMU
static void show_title(const uint32_t hdr[16], uint32_t size) {
    char title[21], shown[32];
    memcpy(title, (const uint8_t *)hdr + 0x20, 20);
    title[20] = 0;
    printf("cartboot: \"%s\", %lu MB\n", title, (unsigned long)(size >> 20));
    loading_cart_title((const uint8_t *)hdr, shown, sizeof shown);
    loading_open(shown, size);
    vmulcd_show(shown);
}

void boot_load(uint8_t *rdram) {
    if (!cart_link_is_open() && !cart_link_open()) play_abort("no cart reader answered");
    uint32_t size = cart_rom_size();
    if (!size) play_abort("the reader's firmware doesn't report the ROM size");
    uint32_t hdr[16];
    if (!cart_read(0, hdr, sizeof hdr)) play_abort("the cart header didn't read");
    show_title(hdr, size);
    loading_progress(CB_STAGE_XLATE, &xl_funcs_done, &cb_out.nfuncs, 0);
    loading_progress(CB_STAGE_XC_GET, &reader_xc_done, &reader_xc_total, 10);
    reader_src(&cb_src, size, &cb_out);
    const size_t stack = 0x40000;  // xlate's working set is mostly static, but its recursion and frames are deep
    void *sp = memalign(32, stack);
    cartboot_spill_open(rdram, RDRAM_BYTES);
    kthread_attr_t attr = { .stack_size = stack, .stack_ptr = sp, .label = "cartboot" };
    g_loading = true;
    kthread_t *t = thd_create_ex(&attr, cb_thread, rdram);
    void *err = 0;
    while (t && !cb_done) {
        loading_draw();
        thd_sleep(100);
    }
    if (!t || thd_join(t, &err) < 0) err = "no cartboot thread";
    g_loading = false;
    free(sp);
    if (err) play_abort((const char *)err);
    if (cb_st.cache_stored) commit_wait();
    reader_upload_start(cb_st.text_crc);
    print_stats(&cb_st);
    loading_stage(LOADING_START);
    loading_draw();
}

void boot_rom_cache(void) {
    play_heap_to_romcache(ovl_work_bytes());  // an overlay's translation needs that whenever the game loads one
    printf("play: ROM cache %lu KB, overlay arena %lu KB, %lu KB kept for translating overlays\n",
           (unsigned long)(romcache_bytes() >> 10), (unsigned long)(ovl_arena_bytes() >> 10),
           (unsigned long)(ovl_work_bytes() >> 10));
}

void boot_rom_poll(void) {
    void *mem;
    uint32_t len;
    if (reader_upload_spare(&mem, &len)) romcache_add(mem, len);  // the play thread owns the cache
}

// The guest function holding pc, by a linear search: the heap is the ROM cache's by the time a hang is reported
static uint32_t guest_fn(uint32_t pc) {
    uint32_t off = pc - (uint32_t)cb_out.text;
    if (off >= cb_out.len) return rgn_any_guest_fn(pc);
    return os_fn_below(cb_out.funcs, cb_out.func_off, 1, cb_out.nfuncs, off, NULL);
}

// A hang at the deadline: where the n64 thread sits in translated code, sampled, then the OS threads
static void on_deadline(void) {
    for (int k = 0; k < 8 && n64_thd; k++) {
        uint32_t pc = n64_thd->context.pc, pr = n64_thd->context.pr;
        printf("stuck: pc %08lx in f_%08lX, pr %08lx in f_%08lX\n", (unsigned long)pc, (unsigned long)guest_fn(pc),
               (unsigned long)pr, (unsigned long)guest_fn(pr));
        thd_sleep(37);
    }
    os_dump();
}

static void on_fault(uint32_t pc, uint32_t pr) {
    printf("fault: pc in f_%08lX, pr in f_%08lX\n", (unsigned long)guest_fn(pc), (unsigned long)guest_fn(pr));
    os_dump();
}

void boot_watch(kthread_t *n64) {
    n64_thd = n64;
    dcmmu_on_fault = on_fault;
    dc_exit_on_deadline = on_deadline;
}
