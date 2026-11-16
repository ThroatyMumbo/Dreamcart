#include <kos.h>
#include "sim.h"

void sim_puts(const char *s) {
    printf("%s", s);
}

void sim_hex(const char *label, uint32_t v) {
    printf("%s%08lx ", label, (unsigned long)v);
}

void sim_exit(int code) {
    printf("bench: abort %d\n", code);
    fflush(stdout);
    arch_abort();
}

uint32_t crc32_word(uint32_t crc, uint32_t w) {
    for (int b = 24; b >= 0; b -= 8) {
        crc ^= (w >> b) & 0xFF;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return crc;
}
