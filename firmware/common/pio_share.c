#include "pio_share.h"

static const pio_tenant_t *g_tenant[NUM_PIOS];

void pio_share_enter(PIO pio, const pio_tenant_t *t) {
    uint i = pio_get_index(pio);
    if (g_tenant[i] == t) return;
    if (g_tenant[i]) g_tenant[i]->unload();
    g_tenant[i] = t;
    if (t) t->load();
}

void pio_share_leave(PIO pio, const pio_tenant_t *t) {
    uint i = pio_get_index(pio);
    if (g_tenant[i] != t || !t) return;
    t->unload();
    g_tenant[i] = NULL;
}
