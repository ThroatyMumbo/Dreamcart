#ifndef TMU1_H
#define TMU1_H

#include <kos.h>

// TMU1, shared: it ticks at the highest rate asked for, and each tick calls every subscriber in the order they came
void tmu1_use(void (*tick)(irq_context_t *ctx), uint32_t hz);

#endif
