#include <kos.h>
#include <dc/biosfont.h>
#include <dc/video.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "loading.h"

#define ROW0 96
#define ROW_H 26
#define ROW_LAST 408  // the elapsed line sits below
#define NAME_X 40
#define VAL_X 340
#define BAR_X 484
#define BAR_W 120
#define BAR_H 14
#define ELAPSED_Y 448
#define WHITE 0xFFFFu  // bfont takes the framebuffer's RGB565 in 16-bit modes
#define GRAY 0x8410u
#define BAR_FILL 0x2D7Fu

static const char *const names[LOADING_N] = {
    [CB_STAGE_CACHE] = "Checking the cache",
    [CB_STAGE_TABLES] = "Reader scanning the ROM",
    [CB_STAGE_SCAN] = "Finding functions",
    [CB_STAGE_SIGS] = "Matching libultra",
    [CB_STAGE_META] = "Building metadata",
    [CB_STAGE_STORE] = "Sending the cache",
    [CB_STAGE_IMAGE] = "Building the boot image",
    [CB_STAGE_XC_GET] = "Reading the SD cache",
    [CB_STAGE_XLATE] = "Translating",
    [CB_STAGE_XC_PUT] = "Packing for the SD cache",
    [CB_STAGE_LINK] = "Linking",
    [LOADING_COMMIT] = "Reader storing the cache",
    [LOADING_START] = "Starting the game",
};

static struct { uint8_t stage; uint64_t t0; } rows[(ROW_LAST - ROW0) / ROW_H + 1];
static volatile unsigned nrows;
static uint64_t open_ms;
static unsigned drawn_done, current = ~0u, spin;
static int bar_fill;
static bool bar_framed;
static char shown_val[12], shown_elapsed[20];  // VRAM writes are slow: only changed characters are redrawn
static struct { const volatile uint32_t *done, *total; unsigned shift; } prog[LOADING_N];

static void text(int x, int y, uint16_t c, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void text(int x, int y, uint16_t c, const char *fmt, ...) {
    char buf[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    bfont_set_foreground_color(c);
    bfont_draw_str_vram_fmt(x, y, true, "%s", buf);
}

// shown holds cap characters already on screen at x, y; zeroed, it all gets drawn
static void text_diff(char *shown, size_t cap, int x, int y, uint16_t c0, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
static void text_diff(char *shown, size_t cap, int x, int y, uint16_t c0, const char *fmt, ...) {
    char buf[24];
    va_list ap;
    va_start(ap, fmt);
    size_t len = (size_t)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    bfont_set_foreground_color(c0);
    for (size_t i = 0; i < cap; i++) {
        char c = i < len ? buf[i] : ' ';
        if (c != shown[i]) bfont_draw_str_vram_fmt(x + 12 * (int)i, y, true, "%c", c);
        shown[i] = c;
    }
}

static void rect(int x, int y, int w, int h, uint16_t c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) vram_s[j * 640 + i] = c;
}

static void bar(int y, uint32_t done, uint32_t total) {
    int fill = total ? (int)((uint64_t)(BAR_W - 4) * done / total) : 0;
    if (fill > bar_fill) rect(BAR_X + 2 + bar_fill, y + 2, fill - bar_fill, BAR_H - 4, BAR_FILL);
    bar_fill = fill;
}

static bool minor_word(const char *w, size_t n) {
    static const char *const minor[] = { "A", "AN", "AND", "AT", "FOR", "IN", "OF", "ON", "THE", "TO" };
    for (size_t i = 0; i < sizeof minor / sizeof *minor; i++)
        if (strlen(minor[i]) == n && !strncmp(w, minor[i], n)) return true;
    return false;
}

static bool insert_space(char *s, size_t at, size_t cap) {
    size_t n = strlen(s);
    if (n + 2 > cap) return false;
    memmove(s + at + 1, s + at, n - at + 1);
    s[at] = ' ';
    return true;
}

static bool letters_before(const char *s, size_t i, size_t n) {
    if (i < n) return false;
    while (n) if (!isalpha((unsigned char)s[i - n--])) return false;
    return true;
}

// Only for a name with no spaces: a number after 3+ letters gets one before it; "F1", "XG2" and "3D" stay whole
static void space_words(char *s, size_t cap) {
    if (strchr(s, ' ')) return;
    for (size_t i = 0; s[i]; i++)
        if (i && isdigit((unsigned char)s[i]) && letters_before(s, i, 3) && insert_space(s, i, cap)) i++;
}

void loading_title_case(char *s, size_t cap) {
    space_words(s, cap);
    bool lower = false, letter = false;
    for (const char *p = s; *p; p++) lower |= islower((unsigned char)*p), letter |= isalpha((unsigned char)*p);
    if (lower || !letter) return;
    bool first = true;
    for (char *w = s; *w;) {
        if (!isalnum((unsigned char)*w)) {
            w++;
            continue;
        }
        char *e = w;
        while (isalnum((unsigned char)*e) || *e == '\'') e++;
        size_t n = (size_t)(e - w);
        bool roman = strspn(w, "IVX") >= n, digit = isdigit((unsigned char)*w);
        if (!roman && !digit) {
            bool down = !first && minor_word(w, n);
            for (size_t i = down ? 0 : 1; i < n; i++) w[i] = (char)tolower((unsigned char)w[i]);
        }
        first = false;
        w = e;
    }
}

void loading_cart_title(const uint8_t *hdr, char *out, size_t cap) {
    char t[21];
    for (int i = 0; i < 20; i++) t[i] = hdr[0x20 + i] >= 0x20 && hdr[0x20 + i] < 0x7F ? (char)hdr[0x20 + i] : ' ';
    t[20] = 0;
    for (int i = 19; i >= 0 && t[i] == ' '; i--) t[i] = 0;
    snprintf(out, cap, "%s", t);
    loading_title_case(out, cap);
}

void loading_open(const char *title, uint32_t rom_bytes) {
    vid_set_mode(DM_640x480, PM_RGB565);
    memset(vram_s, 0, 640 * 480 * 2);
    bfont_set_encoding(BFONT_CODE_ISO8859_1);
    bfont_set_background_color(0);
    open_ms = timer_ms_gettime64();
    text(NAME_X, 40, WHITE, "Loading  %s", title);
    text(VAL_X + 144, 40, GRAY, "%3lu MB", (unsigned long)(rom_bytes >> 20));
}

void loading_stage(int stage) {
    unsigned n = nrows;
    if (ROW0 + (int)n * ROW_H > ROW_LAST) return;
    rows[n].stage = (uint8_t)stage;
    rows[n].t0 = timer_ms_gettime64();
    nrows = n + 1;
}

void loading_progress(int stage, const volatile uint32_t *done, const volatile uint32_t *total, unsigned shift) {
    prog[stage].done = done, prog[stage].total = total, prog[stage].shift = shift;
}

void loading_draw(void) {
    unsigned n = nrows;
    uint64_t now = timer_ms_gettime64();
    for (; drawn_done + 1 < n; drawn_done++) {
        int y = ROW0 + (int)drawn_done * ROW_H;
        rect(VAL_X, y, 640 - VAL_X, BFONT_HEIGHT, 0);
        text(NAME_X, y, GRAY, "%s", names[rows[drawn_done].stage]);
        text(VAL_X, y, GRAY, "%6lu ms", (unsigned long)(rows[drawn_done + 1].t0 - rows[drawn_done].t0));
    }
    if (n) {
        int y = ROW0 + (int)(n - 1) * ROW_H, s = rows[n - 1].stage;
        uint32_t t = prog[s].total ? *prog[s].total : 0;
        if (current != n - 1) {
            current = n - 1;
            text(NAME_X, y, WHITE, "%s", names[s]);
            memset(shown_val, 0, sizeof shown_val);
            bar_framed = false;
        }
        if (t) {
            if (!bar_framed) {
                bar_framed = true;
                bar_fill = 0;
                rect(BAR_X, y + (BFONT_HEIGHT - BAR_H) / 2, BAR_W, BAR_H, GRAY);
                rect(BAR_X + 2, y + (BFONT_HEIGHT - BAR_H) / 2 + 2, BAR_W - 4, BAR_H - 4, 0);
            }
            uint32_t d = *prog[s].done;
            d = d < t ? d : t;
            unsigned sh = prog[s].shift;
            text_diff(shown_val, sizeof shown_val, VAL_X, y, WHITE, sh ? "%4lu/%lu KB" : "%5lu/%lu",
                      (unsigned long)(d >> sh), (unsigned long)(t >> sh));
            bar(y + (BFONT_HEIGHT - BAR_H) / 2, d, t);
        } else {
            uint64_t ms = now - rows[n - 1].t0;
            text_diff(shown_val, sizeof shown_val, VAL_X, y, WHITE, "%4lu.%lu s %c", (unsigned long)(ms / 1000),
                      (unsigned long)(ms / 100 % 10), "|/-\\"[spin++ & 3]);
        }
    }
    text_diff(shown_elapsed, sizeof shown_elapsed, VAL_X, ELAPSED_Y, GRAY, "elapsed %3lu.%lu s",
              (unsigned long)((now - open_ms) / 1000), (unsigned long)((now - open_ms) / 100 % 10));
}
