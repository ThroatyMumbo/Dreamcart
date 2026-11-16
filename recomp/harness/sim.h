#ifndef SIM_H
#define SIM_H

#include "htypes.h"

void sim_puts(const char *s);
void sim_hex(const char *label, uint32_t v);
void sim_exit(int code);
int sim_open_wr(const char *path);
int sim_write(int fd, const void *p, uint32_t n);
int sim_close(int fd);
uint32_t crc32_word(uint32_t crc, uint32_t w);

#endif
