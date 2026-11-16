#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio.h"
#include "pico/stdio_uart.h"
#include "hardware/uart.h"
#include "fault.h"
#include "console.h"

static struct { const console_cmd_t *cmds; unsigned n; } g_tables[CONSOLE_MAX_TABLES];
static unsigned g_ntables;

void console_add(const console_cmd_t *cmds, unsigned n) {
    if (g_ntables < CONSOLE_MAX_TABLES) { g_tables[g_ntables].cmds = cmds; g_tables[g_ntables++].n = n; }
}

static void help(void) {
    for (unsigned t = 0; t < g_ntables; t++)
        for (unsigned i = 0; i < g_tables[t].n; i++) printf("%s\n", g_tables[t].cmds[i].help);
    printf("?          this\n");
}

static void run(const char *line) {
    if (!line[0]) return;
    const char *arg = line + 1;
    while (*arg == ' ') arg++;
    if (line[0] == '?') { help(); return; }
    for (unsigned t = 0; t < g_ntables; t++) {
        for (unsigned i = 0; i < g_tables[t].n; i++) {
            if (g_tables[t].cmds[i].key == line[0]) { g_tables[t].cmds[i].run(arg); return; }
        }
    }
    printf("unknown, ? for help\n");  // short: a long reply couples glitches back into RX
}

// The UART's own TX couples glitches into RX (GP40 next to GP41): what came in during a reply is dropped
static void rx_drain(void) {
    while (uart_is_readable(uart_default)) (void)uart_getc(uart_default);
}

void console_poll(void) {
    static char line[64];
    static int n = 0;
    static bool prompted = false, noisy = false;

    if (!prompted) {
        MARK(10);
        printf("> ");
        stdio_flush();
        uart_tx_wait_blocking(uart_default);
        rx_drain();
        prompted = true;
    }
    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT) return;
    if (c == '\r' || c == '\n') {
        line[n] = 0;
        n = 0;
        printf("\n");
        if (noisy) printf("dropped, the line had glitch bytes\n");
        else run(line);
        noisy = false;
        prompted = false;
        return;
    }
    if (c == 8 || c == 127) { if (n) { n--; printf("\b \b"); } return; }
    if (c < 0x20 || c > 0x7E) { noisy = true; return; }
    if (n < (int)sizeof line - 1) { line[n++] = (char)c; putchar(c); }
}

void console_wait_enter(void) {
    sleep_ms(50);  // the command's line may still have its LF in flight
    while (getchar_timeout_us(0) != PICO_ERROR_TIMEOUT) {}
    for (;;) {
        int c = getchar_timeout_us(100000);
        if (c == '\r' || c == '\n') return;
    }
}

void console_raw_begin(const char *tag, uint32_t total) {
    printf("%s %lu\n", tag, (unsigned long)total);
    stdio_flush();
    stdio_set_translate_crlf(&stdio_uart, false);
}

void console_raw_end(void) {
    fflush(stdout);
    stdio_set_translate_crlf(&stdio_uart, true);
}

void console_raw_dump(const char *tag, uint32_t total, void (*read)(uint32_t off, void *dst, uint32_t n)) {
    static uint8_t buf[512] __attribute__((aligned(4)));   // readers DMA into it
    console_raw_begin(tag, total);
    for (uint32_t off = 0; off < total; off += sizeof buf) {
        uint32_t n = total - off < sizeof buf ? total - off : sizeof buf;
        read(off, buf, n);
        fwrite(buf, 1, n, stdout);
    }
    console_raw_end();
}

void console_hexdump(const uint8_t *d, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (i % 16 == 0) printf("\n%04lX  ", (unsigned long)i);
        printf("%02X ", d[i]);
    }
    printf("\n");
}

void console_print_size(const char *what, uint32_t bytes) {
    if (bytes >= (1u << 20) && !(bytes % (1u << 17))) printf("%s%lu Mbit", what, (unsigned long)(bytes >> 17));
    else printf("%s%lu KB", what, (unsigned long)(bytes >> 10));
}
