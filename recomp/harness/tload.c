// The linker's job for xlate's output, done in RAM: every relocation is a 32-bit absolute word
#include "tload.h"

#include <string.h>

#include "tmpl.h"

extern uint32_t t_budget;
void t_tick(struct tctx *c);
void t_idle(struct tctx *c);
uint32_t t_lookup(uint32_t vram);
void t_untranslated(uint32_t vram);
void t_unsupported(uint32_t vram);

static const TSym fixed[] = {
    {"_t_budget", &t_budget},         {"_t_tick", (void *)t_tick},
    {"_t_generic", (void *)t_generic}, {"_t_external", (void *)t_external},
    {"_t_idle", (void *)t_idle},       {"_t_lookup", (void *)t_lookup},
    {"_t_badjump", (void *)t_badjump}, {"_t_untranslated", (void *)t_untranslated},
    {"_t_unsupported", (void *)t_unsupported}, {"_t_rdram_bias", &t_rdram_bias},
};

static void *find(const char *name) {
    for (size_t i = 0; i < sizeof fixed / sizeof *fixed; i++)
        if (!strcmp(fixed[i].name, name)) return fixed[i].addr;
    for (unsigned i = 0; i < t_nhook_syms; i++)
        if (!strcmp(t_hook_syms[i].name, name)) return t_hook_syms[i].addr;
    return NULL;
}

// "_t_XXXXXXXX": a function vram the translator jumped to without a function there
static int vram_name(const char *s, uint32_t *v) {
    if (strncmp(s, "_t_", 3) || strlen(s) != 11) return 0;
    uint32_t x = 0;
    for (int i = 3; i < 11; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        x = x << 4 | (uint32_t)d;
    }
    *v = x;
    return 1;
}

// und[i] for each undefined name: a runtime symbol, a function lookup gives by vram, else 0 for a stub
static const char *resolve(const XlOut *o, uint32_t *und, uint32_t (*lookup)(uint32_t), uint32_t *nstub) {
    static char err[96];
    *nstub = 0;
    for (uint32_t i = 0; i < o->nund; i++) {
        uint32_t v;
        if (!(und[i] = (uint32_t)(uintptr_t)find(o->und[i])) && !vram_name(o->und[i], &v)) {
            strcpy(err, "tload: no runtime symbol ");
            strncat(err, o->und[i], sizeof err - strlen(err) - 1);
            return err;
        }
        if (!und[i] && lookup) und[i] = lookup(v);
        if (!und[i]) (*nstub)++;
    }
    return NULL;
}

// Per unknown vram, xlate's dispatch() in 24 bytes: the code is looked up (translated if it is run-time code) when
// first called. sts.l pr,@-r15; mov.l @(16),r4; mov.l @(20),r1; jsr @r1; nop; lds.l @r15+,pr; jmp @r0; nop
#define STUB TLOAD_STUB
static void write_stubs(const XlOut *o, uint32_t *und, uint16_t *st) {
    for (uint32_t i = 0, k = 0; i < o->nund; i++) {
        uint32_t v;
        if (und[i] || !vram_name(o->und[i], &v)) continue;
        uint16_t *s = st + STUB / 2 * k++;
        static const uint16_t code[8] = {0x4F22, 0xD403, 0xD103, 0x410B, 0x0009, 0x4F26, 0x402B, 0x0009};
        memcpy(s, code, sizeof code);
        uint32_t words[2] = {v, (uint32_t)(uintptr_t)t_lookup};
        memcpy(s + 8, words, 8);
        und[i] = (uint32_t)(uintptr_t)s;
    }
}

static const char *relocate(const XlOut *o, uint8_t *base, const uint32_t *und) {
    for (uint32_t i = 0; i < o->nrel; i++) {
        const XlReloc *r = &o->rel[i];
        uint32_t *w = (uint32_t *)(base + r->off);
        if (r->off & 3) return "tload: relocation not on a word";
        switch (r->kind) {
        case XL_SYM_SECTION: *w += (uint32_t)(uintptr_t)base; break;
        case XL_SYM_FUNC: *w = (uint32_t)(uintptr_t)(base + o->func_off[r->id]); break;
        case XL_SYM_UND: *w = und[r->id]; break;
        default: return "tload: relocation of an unknown kind";
        }
    }
    return NULL;
}

uint32_t tload_blob_size(const XlOut *o, uint32_t (*lookup)(uint32_t), uint32_t *und) {
    uint32_t nstub;
    if (resolve(o, und, lookup, &nstub)) return 0;
    return ((o->len + 3) & ~3u) + STUB * nstub;
}

const char *tload_blob(const XlOut *o, uint8_t *base, uint32_t (*lookup)(uint32_t), uint32_t *und) {
    uint32_t nstub;
    const char *err = resolve(o, und, lookup, &nstub);
    if (err) return err;
    memcpy(base, o->text, o->len);
    write_stubs(o, und, (uint16_t *)(base + ((o->len + 3) & ~3u)));
    return relocate(o, base, und);
}

const char *tload(XlOut *o, void *(*alloc)(size_t), uint8_t **stubs, uint32_t *stub_len) {
    uint32_t *und = alloc(o->nund * 4 + 4), nstub;
    const char *err = resolve(o, und, NULL, &nstub);
    if (err) return err;
    uint16_t *st = nstub ? alloc(STUB * nstub) : NULL;
    *stubs = (uint8_t *)st, *stub_len = STUB * nstub;
    write_stubs(o, und, st);
    uint8_t *base = o->text;
    if ((err = relocate(o, base, und))) return err;
    t_functab_p = (const uint32_t *)(base + o->functab_off);
    t_functab_n = *(const uint32_t *)(base + o->functab_count_off);
    t_ovtab_p = o->ovtab, t_ovtab_n = o->novl;
    t_call_fn = (void (*)(void (*)(void), struct tctx *))(base + o->call_off);
    return NULL;
}
