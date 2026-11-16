#include "tmpl.h"
#include "calls.h"

uint32_t t_rdram_bias;

void t_lookup_init(void);

void call_init(void) {
    t_lookup_init();
}
