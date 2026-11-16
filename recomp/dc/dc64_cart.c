#include "dc64_cart.h"

#include <kos.h>
#include <stdio.h>
#include <string.h>

#include "cartlink.h"
#include "loading.h"
#include "vmupak.h"

struct dc64_cart dc64_cart;

static kthread_t *g_thd;
static volatile int g_done, g_next;
static char g_note[64];  // the job's own; the UI copies it out in dc64_job_end
static int g_pak_free = -1;

static const char *region_name(char c) {
    switch (c) {
    case 'E': return "North America";
    case 'J': return "Japan";
    case 'P': case 'X': case 'Y': return "Europe";
    case 'D': return "Germany";
    case 'F': return "France";
    case 'I': return "Italy";
    case 'S': return "Spain";
    case 'U': return "Australia";
    case 'A': return "Asia";
    case 'N': return "Canada";
    case 'K': return "Korea";
    case 'B': return "Brazil";
    default: return "region ?";
    }
}

bool dc64_cart_save_ok(void) {
    return dc64_cart.save == 0 || dc64_cart.save == 512 || dc64_cart.save == 2048 || dc64_cart.save == 131072;
}

int dc64_pak_free(void) { return g_pak_free; }

static void read_info(void) {
    struct dc64_cart *c = &dc64_cart;
    uint8_t h[64];
    memset(c, 0, sizeof *c);
    if (cart_read(0, h, sizeof h)) {
        loading_cart_title(h, c->title, sizeof c->title);
        for (int i = 0; i < 4; i++) c->code[i] = h[0x3B + i] >= 0x20 && h[0x3B + i] < 0x7F ? (char)h[0x3B + i] : '?';
        c->region = region_name((char)h[0x3E]);
        c->version = h[0x3F];
    }
    if (!c->title[0]) strcpy(c->title, "(untitled)");
    if (!c->region) c->region = "region ?";
    c->rom_mb = cart_rom_size() >> 20;
    c->save = cart_save_size();
    uint32_t w[N64_MI_CACHE + 1];
    c->cache = cart_media_info(w, N64_MI_CACHE + 1) ? w[N64_MI_CACHE] : 0;
    printf("dc64: cart \"%s\" %s, %lu MB, save %lu B, scan cache %lu B\n", c->title, c->code, (unsigned long)c->rom_mb,
           (unsigned long)c->save, (unsigned long)c->cache);
}

static int probe_state(int r) {
    if (r == CART_LINK_OK) {
        read_info();
        return ST_READY;
    }
    printf("dc64: %s\n", r == CART_LINK_NO_CART ? "no cartridge" : "no cart reader");
    return r == CART_LINK_NO_CART ? ST_NOCART : ST_NOREADER;
}

static void *job_main(void *arg) {
    int j = (int)(intptr_t)arg, next;
    g_note[0] = 0;
    if (j == J_OFF) {
        if (cart_power_off()) next = ST_OFF;
        else {
            strcpy(g_note, "The reader didn't power the cart off");
            next = probe_state(cart_link_probe());
        }
    } else if (j == J_ON) {
        next = probe_state(cart_power_on());
    } else if (j == J_PAK) {
        g_pak_free = vmupak_short();
        next = g_pak_free >= 0 ? ST_PAK : ST_GO;
    } else {
        next = probe_state(cart_power_on());  // a cart left off by an exit mid-swap comes back on
    }
    g_next = next;
    g_done = 1;
    return NULL;
}

void dc64_job_start(enum dc64_job j) {
    g_done = 0;
    g_thd = thd_create(0, job_main, (void *)(intptr_t)j);
    if (!g_thd) job_main((void *)(intptr_t)j);
}

bool dc64_job_running(void) { return g_thd && !g_done; }

int dc64_job_end(char *note, size_t cap) {
    if (!g_done) return -1;
    if (g_thd) {
        thd_join(g_thd, NULL);
        g_thd = NULL;
    }
    g_done = 0;
    if (g_note[0]) snprintf(note, cap, "%s", g_note);
    return g_next;
}
