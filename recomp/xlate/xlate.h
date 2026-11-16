#ifndef XLATE_H
#define XLATE_H

#include <stddef.h>
#include <stdint.h>

enum { XL_SYM_SECTION, XL_SYM_FUNC, XL_SYM_DEF, XL_SYM_UND };
enum { XL_DEF_FUNCTAB, XL_DEF_FUNCTAB_COUNT, XL_DEF_CALL, XL_DEF_ALIAS };

typedef struct {
    uint32_t off;
    uint8_t kind;
    uint32_t id;  // FUNC: index into funcs; DEF: XL_DEF_* (+ alias index); UND: index into und names
} XlReloc;

typedef struct {
    uint8_t *text;
    uint32_t len, cap;
    XlReloc *rel;
    uint32_t nrel, caprel;
    uint32_t nfuncs;
    const uint32_t *funcs;       // vram of each function, in emission order
    uint32_t *func_off;          // .text offset of each _t_XXXXXXXX
    uint32_t nalias;
    const char **alias_name;     // _t_<entry> -> func_off of alias_func
    uint32_t *alias_func;
    uint32_t functab_off, functab_count_off, call_off;
    uint32_t nund;
    char **und;
    uint32_t novl;
    uint32_t *ovtab;             // per overlay: run-time vram, size, ROM offset, key of that vram
} XlOut;

typedef struct {
    uint32_t mips, generic, stubs, hook_native, hook_noop, hook_unsupported;
    uint32_t iterations, max_items, max_ops;
    uint64_t us_translate, us_layout, us_emit;
} XlStats;

int xl_run(const uint8_t *meta, size_t len, int level, XlOut *out, XlStats *st);
// raw[i] = 1 when meta's function i takes every register (a bal target, or it reads registers no call passes)
uint32_t xl_raw_pass(const uint8_t *meta, size_t len, uint8_t *raw);
void xl_free_out(XlOut *o);  // everything xl_run allocated into o; alias_name points into the meta
void xl_release(void);  // xlate's own copy of the last meta and its scratch; o's alias_name dies with it
extern volatile uint32_t xl_funcs_done;  // of out->nfuncs, for a progress display on another thread
uint32_t xl_crc32(uint32_t crc, const void *buf, size_t len);
uint32_t xl_reloc_crc(const XlOut *o);
const char *xl_sym_name(const XlOut *o, const XlReloc *r, char *buf);

uint64_t xl_now_us(void);
void xl_log(const char *fmt, ...);
void *xl_alloc(size_t n);
void *xl_realloc(void *p, size_t n);
void xl_free(void *p);
void xl_die(const char *msg);

#endif
