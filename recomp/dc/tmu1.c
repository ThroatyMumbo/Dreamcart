#include "tmu1.h"

#define SUBS 4

static void (*subs[SUBS])(irq_context_t *ctx);
static uint32_t nsubs, rate;

static void tick(irq_t code, irq_context_t *ctx, void *data) {
    (void)code, (void)data;
    timer_clear(TMU1);
    for (uint32_t i = 0; i < nsubs; i++) subs[i](ctx);
}

void tmu1_use(void (*fn)(irq_context_t *ctx), uint32_t hz) {
    if (nsubs == SUBS) return;
    int old = irq_disable();
    subs[nsubs++] = fn;
    irq_restore(old);
    if (hz <= rate) return;
    rate = hz;
    irq_set_handler(EXC_TMU1_TUNI1, tick, NULL);
    timer_prime(TMU1, rate, 1);
    timer_start(TMU1);
}
