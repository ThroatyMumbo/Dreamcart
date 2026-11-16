#include <kos.h>
#include <dc/biosfont.h>
#include <dc/maple/controller.h>
#include <dc/pvr.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <malloc.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dc64_cart.h"
#include "dc64_menu.h"
#include "dc64_restart.h"
#include "dc64_sfx.h"
#include "dc_exit.h"
#include "vmulcd.h"
#include "vmupak.h"

#define TXR_W      512
#define TXR_H      512
#define SLOT_H     BFONT_HEIGHT
#define SLOT_CHARS (TXR_W / BFONT_THIN_WIDTH)
#define CELL       16  // a bold glyph's texture cell: 12 font pixels, 1 grown, 3 clear
#define CELL_PAD   2
#define TITLE_ADVANCE 14  // font pixels per title letter: a bold glyph is up to 13 wide
#define FACE       (CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START)
#define FADE_S     0.35f
#define NITEMS     3
#define ITEM_Y     344.0f
#define ITEM_H     36.0f
#define RETRY_MS   1000
#define BUSY_SHOW_MS 250  // a reader job quicker than this leaves the screen as it was, so it doesn't flash

#define Z_BG    1.0f
#define Z_TILE  2.0f
#define Z_PANEL 3.0f
#define Z_HI    4.0f
#define Z_TEXT  5.0f
#define Z_FADE  9.0f

enum { S_TITLE, S_NAME, S_L0, S_L1, S_L2, S_ITEM0, S_ITEM1, S_ITEM2, S_BUSY, NSLOT };
enum act { A_NONE, A_START, A_SWAP, A_RETRY, A_ON, A_QUIT, A_ANYWAY, A_CANCEL };

static pvr_ptr_t g_txr;
static pvr_poly_hdr_t g_hdr_op, g_hdr_col, g_hdr_lin, g_hdr_pt;
static uint16_t g_row[SLOT_H * TXR_W] __attribute__((aligned(32)));
static struct { char s[SLOT_CHARS + 1]; int len; bool set; } g_slot[NSLOT];

static const char *g_busy_msg;
static uint64_t g_job_t0;
static char g_note[SLOT_CHARS + 1];
static uint32_t g_prev_buttons, g_chord;
static int g_stick_dir;

// The BIOS font's strokes are one pixel wide: bold grows each one a pixel right and down, and gives each glyph its
// own CELL-wide cell, so neither that nor bilinear filtering reaches the next letter
static void slot_render(int s, bool bold, const char *str) {
    if (g_slot[s].set && !strcmp(str, g_slot[s].s)) return;
    g_slot[s].set = true;
    snprintf(g_slot[s].s, sizeof g_slot[s].s, "%s", str);
    g_slot[s].len = (int)strlen(g_slot[s].s);
    memset(g_row, 0, sizeof g_row);
    if (!bold) bfont_draw_str_ex(g_row, TXR_W, 0xFFFF, 0x0000, 16, true, g_slot[s].s);
    for (int i = 0; bold && i < g_slot[s].len && (i + 1) * CELL <= TXR_W; i++) {
        char one[2] = { g_slot[s].s[i], 0 };
        bfont_draw_str_ex(g_row + i * CELL + CELL_PAD, TXR_W, 0xFFFF, 0x0000, 16, true, one);
    }
    if (bold)
        for (int y = SLOT_H - 1; y >= 0; y--)
            for (int x = TXR_W - 1; x >= 0; x--) {
                uint16_t *p = &g_row[y * TXR_W + x];
                if (x && p[-1]) *p = 0xFFFF;
                if (y && p[-TXR_W]) *p = 0xFFFF;
                if (x && y && p[-TXR_W - 1]) *p = 0xFFFF;
            }
    pvr_txr_load(g_row, (uint8_t *)g_txr + s * sizeof g_row, sizeof g_row);
}

static void slot_set(int s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void slot_set(int s, const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    slot_render(s, false, buf);
}

static void vert(float x, float y, float z, float u, float v, uint32_t c, bool eol) {
    pvr_vertex_t vx = { .flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, .x = x, .y = y, .z = z,
                        .u = u, .v = v, .argb = c, .oargb = 0 };
    pvr_prim(&vx, sizeof vx);
}

static void quad_uv(float x, float y, float w, float h, float z, float u0, float v0, float u1, float v1, uint32_t top,
                    uint32_t bot) {
    vert(x, y + h, z, u0, v1, bot, false);
    vert(x, y, z, u0, v0, top, false);
    vert(x + w, y + h, z, u1, v1, bot, false);
    vert(x + w, y, z, u1, v0, top, true);
}

static void quad(float x, float y, float w, float h, float z, uint32_t top, uint32_t bot) {
    quad_uv(x, y, w, h, z, 0, 0, 0, 0, top, bot);
}

static uint32_t mix(uint32_t a, uint32_t b, float f) {
    uint32_t r = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float ca = (float)(a >> sh & 0xFF), cb = (float)(b >> sh & 0xFF);
        r |= (uint32_t)(ca + (cb - ca) * f) << sh;
    }
    return r;
}

static uint32_t with_alpha(uint32_t c, float a) { return (c & 0xFFFFFF) | (uint32_t)(a * 255.0f) << 24; }

static float text_w(int s, float scale) { return (float)(g_slot[s].len * BFONT_THIN_WIDTH) * scale; }

static void text(int s, float x, float y, float scale, uint32_t top, uint32_t bot) {
    int n = g_slot[s].len;
    if (!n) return;
    float v0 = (float)(s * SLOT_H) / TXR_H, v1 = (float)((s + 1) * SLOT_H) / TXR_H;
    quad_uv(x, y, text_w(s, scale), SLOT_H * scale, Z_TEXT, 0, v0, (float)(n * BFONT_THIN_WIDTH) / TXR_W, v1, top, bot);
}

static void text_c(int s, float y, float scale, uint32_t top, uint32_t bot) {
    text(s, floorf((640.0f - text_w(s, scale)) / 2), y, scale, top, bot);
}

static void background(float t) {
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&g_hdr_op, sizeof g_hdr_op);
    float s = 0.5f + 0.5f * sinf(t * 0.25f);
    quad(0, 0, 640, 480, Z_BG, mix(0xFF14206A, 0xFF3A1460, s), mix(0xFF03050E, 0xFF0B0418, s));
    pvr_list_finish();
}

static void tiles(float t) {
    const float cell = 64.0f, sx = t * 18.0f, sy = t * 9.0f;
    float ox = fmodf(sx, cell), oy = fmodf(sy, cell);
    int bx = (int)floorf(sx / cell), by = (int)floorf(sy / cell);
    for (int cy = -1; cy < 9; cy++)
        for (int cx = -1; cx < 11; cx++) {
            int wx = cx - bx, wy = cy - by;
            float a = 0.5f + 0.5f * sinf(t * 1.3f + (float)wx * 0.8f + (float)wy * 0.55f);
            uint32_t c = with_alpha(0x6FA8FF, 0.03f + 0.09f * a);
            quad((float)cx * cell + ox + 6, (float)cy * cell + oy + 6, cell - 12, cell - 12, Z_TILE, c, c);
        }
}

static void panel(float x, float y, float w, float h) {
    quad(x, y, w, h, Z_PANEL, 0xB0121A38, 0xC006091A);
    const uint32_t e = 0x906FA8FF;
    quad(x, y, w, 2, Z_PANEL + 0.1f, e, e);
    quad(x, y + h - 2, w, 2, Z_PANEL + 0.1f, e, e);
    quad(x, y, 2, h, Z_PANEL + 0.1f, e, e);
    quad(x + w - 2, y, 2, h, Z_PANEL + 0.1f, e, e);
}

static void spinner(float cx, float cy, float t) {
    for (int i = 0; i < 8; i++) {
        float ang = (float)i * (2.0f * (float)M_PI / 8.0f), age = fmodf(t * 10.0f - (float)i + 80.0f, 8.0f) / 8.0f;
        uint32_t c = with_alpha(0xBFE0FF, 1.0f - age * 0.85f);
        quad(cx + 16.0f * cosf(ang) - 4, cy + 16.0f * sinf(ang) - 4, 8, 8, Z_HI, c, c);
    }
}

// "DREAMCART 64", one quad per letter on a wave, over its own shadow
static void title(float t) {
    const float sc = 3.0f, cw = TITLE_ADVANCE * sc, ch = SLOT_H * sc;
    int n = g_slot[S_TITLE].len;
    float x0 = floorf((640.0f - (float)n * cw) / 2), y0 = 46.0f, v1 = (float)SLOT_H / TXR_H;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            char c = g_slot[S_TITLE].s[i];
            if (c == ' ') continue;
            float y = y0 + 7.0f * sinf(t * 2.2f - (float)i * 0.45f), x = x0 + (float)i * cw - CELL_PAD * sc;
            float u0 = (float)(i * CELL) / TXR_W, u1 = (float)((i + 1) * CELL) / TXR_W, qw = CELL * sc;
            uint32_t top = 0xFFFFFFFF, bot = 0xFF8FC8FF;
            if (c == '6') top = 0xFFFFD040, bot = 0xFFFF5020;
            if (c == '4') top = 0xFF90FFA0, bot = 0xFF10A8E0;
            float z = Z_TEXT + 0.01f * (float)i;  // overlapping quads at one depth flicker in the translucent sort
            if (pass == 0) quad_uv(x + 5, y + 6, qw, ch, z - 0.5f, u0, 0, u1, v1, 0xA0000010, 0xA0000010);
            else quad_uv(x, y, qw, ch, z, u0, 0, u1, v1, top, bot);
        }
}

static void job_start(enum dc64_job j, const char *msg) {
    g_busy_msg = msg;
    g_job_t0 = timer_ms_gettime64();
    dc64_job_start(j);
}

// D-pad and stick on press; A and Start on release, and only when nothing else was held, so the exit combo acts alone
static int pad_event(void) {
    uint32_t p = dc_exit_pad(), b = p & 0xFFFF, pressed = b & ~g_prev_buttons;
    int y = (int)(p >> 24 & 0xFF) - 128, dir = y < -64 ? -1 : y > 64 ? 1 : 0, ev = 0;
    g_prev_buttons = b;
    if (pressed & CONT_DPAD_UP || (dir < 0 && g_stick_dir >= 0)) ev = 'u';
    if (pressed & CONT_DPAD_DOWN || (dir > 0 && g_stick_dir <= 0)) ev = 'd';
    g_stick_dir = dir;
    g_chord |= b & FACE;
    if (!(b & FACE) && g_chord) {
        if (g_chord == CONT_A) ev = 'a';
        else if (g_chord == CONT_START) ev = 's';
        g_chord = 0;
    }
    return ev;
}

static void pvr_setup(void) {
    pvr_init_params_t params = {
        .opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0 },
        .vertex_buf_size = 256 * 1024,
    };
    pvr_init(&params);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    g_txr = pvr_mem_malloc(TXR_W * TXR_H * 2);
    memset(g_row, 0, sizeof g_row);
    for (int s = 0; s < TXR_H / SLOT_H; s++) pvr_txr_load(g_row, (uint8_t *)g_txr + s * sizeof g_row, sizeof g_row);
    memset(g_slot, 0, sizeof g_slot);
    pvr_poly_cxt_t c;
    pvr_poly_cxt_col(&c, PVR_LIST_OP_POLY);
    c.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&g_hdr_op, &c);
    pvr_poly_cxt_col(&c, PVR_LIST_TR_POLY);
    c.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&g_hdr_col, &c);
    const int fmt = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED;
    pvr_poly_cxt_txr(&c, PVR_LIST_TR_POLY, fmt, TXR_W, TXR_H, g_txr, PVR_FILTER_BILINEAR);
    c.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&g_hdr_lin, &c);
    pvr_poly_cxt_txr(&c, PVR_LIST_TR_POLY, fmt, TXR_W, TXR_H, g_txr, PVR_FILTER_NEAREST);
    c.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&g_hdr_pt, &c);
}

static const char *const act_label[] = { [A_START] = "Start game", [A_SWAP] = "Swap cartridge", [A_RETRY] = "Try again",
                                         [A_ON] = "Power on", [A_QUIT] = "Quit", [A_ANYWAY] = "Play anyway",
                                         [A_CANCEL] = "Cancel" };
// What each state offers, in menu order; ST_BUSY offers nothing
static const enum act by_state[ST_GO + 1][NITEMS] = {
    [ST_READY] = { A_START, A_SWAP, A_QUIT }, [ST_GO] = { A_START, A_SWAP, A_QUIT }, [ST_PAK] = { A_ANYWAY, A_CANCEL },
    [ST_NOCART] = { A_SWAP, A_RETRY, A_QUIT }, [ST_OFF] = { A_ON }, [ST_NOREADER] = { A_QUIT },
};

struct menu {
    int state, sel, n;
    float fade;  // < 0 until the screen starts fading out
    bool quit;
    enum act items[NITEMS];
};

static bool act_enabled(enum act a) { return a != A_START || dc64_cart_save_ok(); }

static void offer(struct menu *m, bool show_busy) {
    m->n = 0;
    for (int i = 0; i < NITEMS; i++) {
        m->items[i] = show_busy ? A_NONE : by_state[m->state][i];
        m->n += m->items[i] != A_NONE;
    }
    if (m->sel >= m->n) m->sel = 0;
}

static enum act menu_input(struct menu *m, bool busy) {
    int ev = pad_event();
    if (m->fade >= 0 || busy) ev = 0;
    if (m->n > 1 && (ev == 'u' || ev == 'd')) {
        m->sel = (m->sel + (ev == 'u' ? m->n - 1 : 1)) % m->n;
        dc64_sfx(SFX_MOVE);
    }
    enum act a = A_NONE;
    if (ev == 'a' && m->n) {
        if (act_enabled(m->items[m->sel])) a = m->items[m->sel];
        else dc64_sfx(SFX_DENY);
    }
    if (ev == 's' && m->state == ST_READY && !busy && dc64_cart_save_ok()) a = A_START;
    return a;
}

static void menu_act(struct menu *m, enum act a) {
    if (a == A_NONE) return;
    dc64_sfx(SFX_SELECT);
    g_note[0] = 0;
    switch (a) {
    case A_START: job_start(J_PAK, "Checking the VMU"); break;
    case A_ANYWAY:
        printf("dc64: start \"%s\", no pak\n", dc64_cart.title);
        m->state = ST_GO;
        m->fade = 0.0f;
        break;
    case A_CANCEL: m->state = ST_READY, m->sel = 0; break;
    case A_QUIT:
        printf("dc64: quit to dcload\n");
        m->quit = true;
        m->fade = 0.0f;
        break;
    case A_SWAP:
        printf("dc64: swap, powering the cart off\n");
        job_start(J_OFF, "Powering the cartridge off");
        break;
    case A_RETRY: job_start(J_PROBE, "Reading the cartridge"); break;
    case A_ON:
        printf("dc64: powering the cart on\n");
        job_start(J_ON, "Powering the cartridge on");
        break;
    case A_NONE: break;
    }
}

static void menu_text(const struct menu *m, bool show_busy) {
    int state = m->state;
    if (state == ST_PAK) {
        slot_set(S_NAME, "VMU is full");
        slot_set(S_L0, "%d block%s free; a Controller Pak needs %u", dc64_pak_free(), dc64_pak_free() == 1 ? "" : "s",
                 VMUPAK_BLOCKS);
        slot_set(S_L1, "Saving to a Controller Pak won't work");
    } else if (state == ST_READY || state == ST_GO) {
        slot_set(S_NAME, "%s", dc64_cart.title);
        slot_set(S_L0, "%s  %s  v1.%u  %lu MB", dc64_cart.code, dc64_cart.region, dc64_cart.version,
                 (unsigned long)dc64_cart.rom_mb);
        if (dc64_cart.save == 0) slot_set(S_L1, "Save: none");
        else if (dc64_cart_save_ok()) slot_set(S_L1, "Save: EEPROM %s", dc64_cart.save == 512 ? "4 Kbit" : "16 Kbit");
        else slot_set(S_L1, "Save: %lu B chip, not supported", (unsigned long)dc64_cart.save);
    } else if (state == ST_NOCART) {
        slot_set(S_NAME, "No cartridge");
        slot_set(S_L0, "Please insert an N64 cartridge");
        slot_set(S_L1, "%s", "");
    } else if (state == ST_OFF) {
        slot_set(S_NAME, "Cartridge off");
        slot_set(S_L0, "Swap out the cartridge");
        slot_set(S_L1, "and press A when ready");
    } else {
        slot_set(S_NAME, "%s", state == ST_NOREADER ? "No cart reader" : "");
        slot_set(S_L0, "%s", "");
        slot_set(S_L1, "%s", "");
    }
    slot_set(S_L2, "%s", g_note);
    for (int i = 0; i < NITEMS; i++) slot_set(S_ITEM0 + i, "%s", i < m->n ? act_label[m->items[i]] : "");
    slot_set(S_BUSY, "%s", show_busy ? g_busy_msg : "");
}

static void menu_draw(const struct menu *m, float t, bool show_busy) {
    pvr_wait_ready();
    pvr_scene_begin();
    background(t);
    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(&g_hdr_col, sizeof g_hdr_col);
    tiles(t);
    panel(70, 156, 500, 172);
    if (m->n) {
        float pulse = 0.5f + 0.5f * sinf(t * 4.0f);
        uint32_t hc = with_alpha(0x4FA0FF, 0.25f + 0.25f * pulse);
        quad(170, ITEM_Y + ITEM_H * (float)m->sel - 6, 300, 36, Z_HI, hc, with_alpha(0x2060C0, 0.3f + 0.2f * pulse));
    }
    if (show_busy) spinner(320, 420, t);
    pvr_prim(&g_hdr_lin, sizeof g_hdr_lin);
    title(t);
    pvr_prim(&g_hdr_pt, sizeof g_hdr_pt);
    text_c(S_NAME, 170, 2.0f, 0xFFFFFFFF, 0xFFA8D0FF);
    text_c(S_L0, 228, 1.0f, 0xFFD8E0F0, 0xFFD8E0F0);
    bool warn = m->state == ST_PAK || (m->state == ST_READY && !dc64_cart_save_ok());
    text_c(S_L1, 256, 1.0f, warn ? 0xFFFF9070 : 0xFFD8E0F0, 0xFFD8E0F0);
    text_c(S_L2, 284, 1.0f, 0xFFFF9070, 0xFFFF9070);
    for (int i = 0; i < m->n; i++) {
        uint32_t c = !act_enabled(m->items[i]) ? 0xFF707888 : i == m->sel ? 0xFFFFFFFF : 0xFFB0C0D8;
        text_c(S_ITEM0 + i, ITEM_Y + ITEM_H * (float)i, 1.0f, c, c);
    }
    text_c(S_BUSY, 380, 1.0f, 0xFFFFE0A0, 0xFFFFC060);
    if (m->fade >= 0) {
        pvr_prim(&g_hdr_col, sizeof g_hdr_col);
        uint32_t k = with_alpha(0x000000, m->fade > 1.0f ? 1.0f : m->fade);
        quad(0, 0, 640, 480, Z_FADE, k, k);
    }
    pvr_list_finish();
    pvr_scene_finish();
}

void dc64_menu(void) {
    bfont_set_encoding(BFONT_CODE_ISO8859_1);
    pvr_setup();
    snd_init();
    dc64_sfx_init();
    slot_render(S_TITLE, true, "DREAMCART 64");
    vmulcd_show("DREAMCART 64");
    printf("dc64: menu%s\n", dc64_boot_count() ? ", back from a game" : "");
    dc64_take_note(g_note, sizeof g_note);

    struct menu m = { .state = ST_BUSY, .fade = -1.0f };
    uint64_t t0 = timer_ms_gettime64(), retry_at = 0, last = t0;
    job_start(J_PROBE, "Looking for the cart reader");
    for (;;) {
        uint64_t now = timer_ms_gettime64();
        float t = (float)(now - t0) / 1000.0f, dt = (float)(now - last) / 1000.0f;
        last = now;
        int next = dc64_job_end(g_note, sizeof g_note);
        if (next >= 0) {
            m.state = next;
            m.sel = 0;
            retry_at = now + RETRY_MS;
            if (m.state == ST_PAK) printf("dc64: VMU full (%d blocks free)\n", dc64_pak_free());
            if (m.state == ST_GO) {
                printf("dc64: start \"%s\"\n", dc64_cart.title);
                m.fade = 0.0f;
            }
        }
        bool busy = dc64_job_running(), show_busy = busy && now - g_job_t0 >= BUSY_SHOW_MS;
        if (!busy && m.state == ST_NOREADER && now >= retry_at) {
            job_start(J_PROBE, "Cart reader not found, still looking");
            busy = dc64_job_running();
        }
        offer(&m, show_busy);
        menu_act(&m, menu_input(&m, busy));
        menu_text(&m, show_busy);
        menu_draw(&m, t, show_busy);
        if (m.fade >= 0 && (m.fade += dt / FADE_S) > 1.0f) break;
    }
    pvr_wait_ready();
    if (m.quit) {
        fflush(stdout);
        arch_abort();
    }
    snd_sfx_unload_all();
    snd_shutdown();  // the ARM back in reset: the game drives the AICA's channels itself
    pvr_mem_free(g_txr);
    pvr_shutdown();
}
