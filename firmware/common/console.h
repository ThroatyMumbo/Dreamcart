#pragma once
#include <stdint.h>

typedef struct {
    char key;
    const char *help;               // the whole help entry, key and arguments included
    void (*run)(const char *arg);   // the rest of the line, leading spaces skipped
} console_cmd_t;

// The UART line console. Unknown keys and '?' print every entry's help, tables in the order added.
#define CONSOLE_MAX_TABLES 4
void console_add(const console_cmd_t *cmds, unsigned n);
void console_poll(void);            // core0 main loop; never blocks

// Raw binary on stdout after a "<tag> <total>" line, with CRLF translation off until console_raw_end.
void console_raw_begin(const char *tag, uint32_t total);
void console_raw_end(void);

// total bytes as a raw block, read 512 at a time
void console_raw_dump(const char *tag, uint32_t total, void (*read)(uint32_t off, void *dst, uint32_t n));

// 16 bytes a line, each led by its offset
void console_hexdump(const uint8_t *d, uint32_t n);

// "<what><n> Mbit" for whole Mbits from 1 MB up, else "<what><n> KB"; no newline
void console_print_size(const char *what, uint32_t bytes);
