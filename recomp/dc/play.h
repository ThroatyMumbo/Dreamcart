// The realtime build's modules: play.c's core, play_stats.c's accounting and the boot
#ifndef PLAY_H
#define PLAY_H

#include <kos.h>
#include <stdint.h>
#include "htypes.h"

extern uint8_t *play_rdram;
void play_abort(const char *why) __attribute__((noreturn));  // back to the menu when there is one, else dcload
void play_load_failed(void);  // arms the restart into the menu, if there is one, for whatever abort comes next
uint32_t play_heap_to_romcache(uint32_t keep);  // the heap past KOS's share and keep, to the ROM cache; returns its bytes

// How the game gets into RDRAM: play_cartboot.c, the inserted cart translated here
uint32_t boot_stack_bytes(void);  // what the n64 thread's stack needs beyond the OS's pool
void boot_load(uint8_t *rdram);   // main thread: opens the cart link and fills RDRAM, or play_abort
void boot_rom_cache(void);        // after romcache_init: the boot's spent memory and the heap go to the ROM cache
void boot_rom_poll(void);         // the n64 thread, before each ROM read
void boot_watch(kthread_t *n64);  // deadline and fault diagnostics
void boot_hang_report(void);      // where the n64 thread sits, sampled, then the OS threads; the exit combo's too

// play_stats.c, all in console time: a report line per second and a line per game frame
extern uint32_t stats_second, stats_frame_no;
void stats_start(void);
void stats_report(void);  // prints once a second has passed since the last report
void stats_report_audio(void);  // the audio path's lines: play_stats_aica.c or play_stats_hle.c
uint64_t stats_report_us(void);  // spent printing reports, all told
void stats_idle(uint64_t us);
void stats_pi_wait(uint64_t us);
void stats_pi(uint32_t len);
void stats_task_start(void);
void stats_task_pause(void);
void stats_gfx_done(uint32_t tris);
void stats_audio_done(void);
void stats_flush(void);  // the buffered frame lines

#endif
