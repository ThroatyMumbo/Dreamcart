#ifndef RCP_H
#define RCP_H

#include "htypes.h"

#define MI_INTR_SP 0x01u
#define MI_INTR_SI 0x02u
#define MI_INTR_AI 0x04u
#define MI_INTR_VI 0x08u
#define MI_INTR_PI 0x10u
#define MI_INTR_DP 0x20u

#define RCP_NO_EVENT 0xFFFFFFFFFFFFFFFFull

uint32_t rcp_rd(uint32_t phys, int size);
void rcp_wr(uint32_t phys, uint32_t v, int size);
uint32_t rcp_mi_pending(void);
uint32_t rcp_mi_mask(void);
void rcp_schedule(unsigned long long when, uint32_t bits);
void rcp_boot_spmem(uint32_t fill, uint32_t imem0);  // DMEM and IMEM all fill, then IMEM word 0, as IPL3 leaves them
void rcp_tick(unsigned long long now);
unsigned long long rcp_next_event(unsigned long long now);
uint32_t rcp_vi_period(void);

// Cart ROM for PI DMA: bytes in z64 order from ROM offset off. size 0 when the backend doesn't know it.
typedef int (*rcp_cart_fn)(uint32_t off, void *dst, uint32_t len);
void rcp_set_cart(rcp_cart_fn read, uint32_t size);
void rcp_set_cart_peek(rcp_cart_fn read);  // rcp_cart_read's backend instead, when PI DMAs' one keeps accounts
int rcp_cart_read(uint32_t off, void *dst, uint32_t len);  // through that backend; 0 with none or on failure
uint32_t rcp_ai_hz(void);  // the AI's output rate from AI_DACRATE
extern uint32_t rcp_ai_dry, rcp_ai_frames;  // buffers that ended with none queued; stereo frames queued
int rcp_rsp_parked(void);  // a graphics task is waiting for the CPU
int rcp_rsp_idle(void);  // runs a parked graphics task on; 0 if there was none
uint32_t rcp_vi_reg(int r);
extern uint32_t rcp_vi_intrs;  // VI interrupts raised; fewer than fields when a long stretch merges several

// What the RCP model tells whoever runs the boot (dc/play.c); NULL: nobody
struct gfx_stats;
struct rcp_hooks {
    void (*pi)(uint32_t rom_off, uint32_t dram, uint32_t len, unsigned long long done_at);
    // Each VI retrace raised: fields since VI started (late retraces count every field they covered)
    void (*vi)(uint32_t field);
    // Before the RSP HLE runs a task (1 graphics, 2 audio); the matching *_task follows when it is done
    void (*task_start)(uint32_t type);
    // task_start opens each run of a task on the CPU; a graphics task that parks or yields closes it here
    void (*task_pause)(uint32_t type);
    // A graphics task the HLE ran: SP memory as it started, SP_PC, its counts and modeled end
    void (*gfx_task)(const uint8_t *spmem, uint32_t pc, const struct gfx_stats *st, unsigned long long done_at);
    // An audio task the HLE ran: its command list, command count and modeled end
    void (*audio_task)(uint32_t list, uint32_t cmds, unsigned long long done_at);
};
extern struct rcp_hooks rcp_hooks;

#endif
