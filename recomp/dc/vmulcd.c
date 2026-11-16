#include <stdio.h>
#include <string.h>
#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <dc/vmu_fb.h>
#include <kos/mutex.h>
#include "vmulcd.h"

#define GLYPH_W 4  // KOS's default vmufb font is 4x6
#define GLYPH_H 6
#define COLS (VMU_SCREEN_WIDTH / GLYPH_W)
#define PITCH 8
#define MAX_LINES (VMU_SCREEN_HEIGHT / PITCH)

static mutex_t lock = MUTEX_INITIALIZER;
static char resting[32];

static void draw(const char *s) {
    char line[MAX_LINES][COLS + 1];
    int n = 0;
    while (n < MAX_LINES) {
        while (*s == ' ') s++;
        if (!*s) break;
        size_t take = strlen(s);
        if (take > COLS) {
            take = COLS;
            for (size_t i = COLS; i > 0; i--)
                if (s[i] == ' ') {
                    take = i;
                    break;
                }
        }
        size_t len = take;
        while (len && s[len - 1] == ' ') len--;
        for (size_t i = 0; i < len; i++) line[n][i] = s[i] >= 0x20 && s[i] < 0x7F ? s[i] : '?';
        line[n++][len] = 0;
        s += take;
    }
    vmufb_t fb;
    vmufb_clear(&fb);
    int y = (VMU_SCREEN_HEIGHT - (PITCH * n - (PITCH - GLYPH_H))) / 2;
    for (int i = 0; i < n; i++, y += PITCH) {
        unsigned w = GLYPH_W * (unsigned)strlen(line[i]);
        vmufb_print_string_into(&fb, NULL, (VMU_SCREEN_WIDTH - w) / 2, (unsigned)y, w, GLYPH_H, 0, line[i]);
    }
    for (int p = 0; p < MAPLE_PORT_COUNT; p++)
        for (int u = 1; u < MAPLE_UNIT_COUNT; u++) {
            maple_device_t *d = maple_enum_dev(p, u);
            if (d && d->info.functions & MAPLE_FUNC_LCD) vmufb_present(&fb, d);
        }
}

void vmulcd_show(const char *text) {
    mutex_lock(&lock);
    snprintf(resting, sizeof resting, "%s", text);
    draw(resting);
    mutex_unlock(&lock);
}

void vmulcd_saving(int on) {
    mutex_lock(&lock);
    draw(on ? "Saving..." : resting);
    mutex_unlock(&lock);
}
