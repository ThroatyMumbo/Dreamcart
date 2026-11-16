// The realtime build's accounting: where each second and each game frame went, in console time
#include <kos.h>
#include <string.h>
#include "play.h"
#include "os.h"
#include "rcp.h"
#include "gfx_tex.h"
#include "gfx_pvr.h"
#include "romcache.h"

#define REPORT_US 1000000u

// Per report window, in microseconds unless named otherwise
static struct {
    uint32_t gfx_tasks, audio_tasks, pi_dmas, pi_bytes, tris;
    uint64_t gfx_us, audio_us, pi_us, idle_us, rep_us;
} w;
// Per game frame (gfx task end to gfx task end), same units
static struct {
    uint32_t audio_tasks, pi_bytes;
    uint64_t audio_us, pi_us, idle_us, rep_us;
} fr;
static uint64_t win_start, task_t0, wait0, frame_end, rep_total, fetched0, frame_fetched0;
static uint64_t gfx_slices_us;  // the runs of a graphics task that parked or yielded before its last
static uint32_t fields0, vi_intrs0, frame_fields, uploads0;
uint32_t stats_second, stats_frame_no;
static char frame_log[4096];
static unsigned frame_log_len;

static uint64_t now_us(void) { return timer_us_gettime64(); }

void stats_flush(void) {
    if (!frame_log_len) return;
    fwrite(frame_log, 1, frame_log_len, stdout);
    frame_log_len = 0;
}

// What drew less or was evicted early, only in the seconds it happened
static void report_gfx_notes(void) {
    static uint32_t forced0, kept0;
    if (gfx_tex_forced != forced0) {
        printf("gfx: %lu textures evicted while still drawn\n", (unsigned long)(gfx_tex_forced - forced0));
        forced0 = gfx_tex_forced;
    }
    if (gfx_pvr_stats.kept_frames != kept0) {
        printf("gfx: %lu frames drew less than the one before; it stays under them\n",
               (unsigned long)(gfx_pvr_stats.kept_frames - kept0));
        kept0 = gfx_pvr_stats.kept_frames;
    }
}

void stats_report(void) {
    uint64_t t = now_us();
    if (t - win_start < REPORT_US) return;
    stats_flush();
    stats_report_audio();
    static uint32_t rows0;
    uint64_t span = t - win_start, pvr_wait = gfx_pvr_stats.wait_us - wait0;
    uint32_t fields = (uint32_t)(os_now() / rcp_vi_period());
    uint64_t busy = w.gfx_us + w.audio_us + w.pi_us + w.idle_us + w.rep_us;
    uint64_t fetched = romcache_stats.fetched_bytes;
    printf("play %2lu: fields %3lu (vi intrs %3lu) frames %2lu audio %2lu | ms gfx %3lu (pvr wait %3lu) audio %3lu pi %3lu (%lu KB, cart %lu KB) "
           "idle %3lu game %3lu | tris/frame %lu fb rows back %lu\n",
           (unsigned long)stats_second, (unsigned long)(fields - fields0), (unsigned long)(rcp_vi_intrs - vi_intrs0),
           (unsigned long)w.gfx_tasks,
           (unsigned long)w.audio_tasks, (unsigned long)(w.gfx_us / 1000), (unsigned long)(pvr_wait / 1000),
           (unsigned long)(w.audio_us / 1000), (unsigned long)(w.pi_us / 1000), (unsigned long)(w.pi_bytes >> 10),
           (unsigned long)((fetched - fetched0) >> 10),
           (unsigned long)(w.idle_us / 1000), (unsigned long)((span > busy ? span - busy : 0) / 1000),
           (unsigned long)(w.gfx_tasks ? w.tris / w.gfx_tasks : 0), (unsigned long)(gfx_pvr_stats.readback_rows - rows0));
    rows0 = gfx_pvr_stats.readback_rows;
    report_gfx_notes();
    fflush(stdout);
    memset(&w, 0, sizeof w);
    w.rep_us = now_us() - t;  // the report's own console I/O is charged to the next window
    fr.rep_us += w.rep_us;
    rep_total += w.rep_us;
    win_start = t;
    fetched0 = fetched;
    wait0 = gfx_pvr_stats.wait_us;
    fields0 = fields;
    vi_intrs0 = rcp_vi_intrs;
    stats_second++;
}

static unsigned long tenths(uint64_t us) { return (unsigned long)((us + 50) / 100); }

// One line per game frame, times in 0.1 ms: fr <n> <fields> <wall> <gfx> <audio> <tasks> <pi> <KB> <cart KB> <idle> <rep> <game> <tris> <uploads>
static void frame_record(uint64_t t, uint64_t gfx_us, uint32_t tris) {
    uint32_t fields = (uint32_t)(os_now() / rcp_vi_period());
    uint64_t wall = t - frame_end, known = gfx_us + fr.audio_us + fr.pi_us + fr.idle_us + fr.rep_us;
    uint64_t fetched = romcache_stats.fetched_bytes;
    if (frame_log_len > sizeof frame_log - 128) stats_flush();
    frame_log_len += (unsigned)snprintf(frame_log + frame_log_len, sizeof frame_log - frame_log_len,
        "fr %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu\n", (unsigned long)stats_frame_no,
        (unsigned long)(fields - frame_fields), tenths(wall), tenths(gfx_us), tenths(fr.audio_us),
        (unsigned long)fr.audio_tasks, tenths(fr.pi_us), (unsigned long)(fr.pi_bytes >> 10),
        (unsigned long)((fetched - frame_fetched0) >> 10), tenths(fr.idle_us), tenths(fr.rep_us),
        tenths(wall > known ? wall - known : 0), (unsigned long)tris, (unsigned long)(gfx_tex_uploads - uploads0));
    memset(&fr, 0, sizeof fr);
    frame_fetched0 = fetched;
    frame_end = t;
    frame_fields = fields;
    uploads0 = gfx_tex_uploads;
    stats_frame_no++;
}

void stats_start(void) { win_start = frame_end = now_us(); }

uint64_t stats_report_us(void) { return rep_total; }

void stats_idle(uint64_t us) {
    w.idle_us += us;
    fr.idle_us += us;
}

void stats_pi_wait(uint64_t us) {
    w.pi_us += us;
    fr.pi_us += us;
}

void stats_pi(uint32_t len) {
    w.pi_dmas++;
    w.pi_bytes += len;
    fr.pi_bytes += len;
}

void stats_task_start(void) { task_t0 = now_us(); }

void stats_task_pause(void) { gfx_slices_us += now_us() - task_t0; }

void stats_gfx_done(uint32_t tris) {
    uint64_t t = now_us(), us = gfx_slices_us + (t - task_t0);
    gfx_slices_us = 0;
    w.gfx_us += us;
    w.gfx_tasks++;
    w.tris += tris;
    frame_record(t, us, tris);
    stats_report();
}

void stats_audio_done(void) {
    uint64_t us = now_us() - task_t0;
    w.audio_us += us;
    w.audio_tasks++;
    fr.audio_us += us;
    fr.audio_tasks++;
}
