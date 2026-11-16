// zlib's CRC-32, and the relocations' CRC that cartboot and dcxlate.py check a translation by
#include <stdio.h>

#include "xl_int.h"

static uint32_t crc_table[256];

uint32_t xl_crc32(uint32_t crc, const void *buf, size_t len) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    const uint8_t *p = buf;
    crc = ~crc;
    while (len--)
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

const char *xl_sym_name(const XlOut *o, const XlReloc *r, char *buf) {
    switch (r->kind) {
    case XL_SYM_SECTION: return ".text";
    case XL_SYM_FUNC: snprintf(buf, 16, "_t_%08X", (unsigned)o->funcs[r->id]); return buf;
    case XL_SYM_UND: return o->und[r->id];
    }
    return "?";
}

uint32_t xl_reloc_crc(const XlOut *o) {
    uint32_t crc = 0;
    char buf[16];
    for (uint32_t i = 0; i < o->nrel; i++) {
        uint8_t off[4] = {o->rel[i].off, o->rel[i].off >> 8, o->rel[i].off >> 16, o->rel[i].off >> 24};
        const char *s = xl_sym_name(o, &o->rel[i], buf);
        crc = xl_crc32(crc, off, 4);
        crc = xl_crc32(crc, s, strlen(s) + 1);
    }
    return crc;
}
