#ifndef TLOAD_H
#define TLOAD_H

#include <stddef.h>
#include <stdint.h>

#include "xlate.h"

typedef struct { const char *name; void *addr; } TSym;

extern const TSym t_hook_syms[];  // tools/gen_sigdb.py: every _h_<hook> in hooks/policy.json
extern const unsigned t_nhook_syms;

#define TLOAD_STUB 24u  // bytes per stub

// Links xlate's output in place and points t_call_fn / t_functab_p at it; jumps to non-functions get t_lookup stubs.
// Returns NULL or what didn't resolve; the caller syncs the caches over o->text and *stubs.
const char *tload(XlOut *o, void *(*alloc)(size_t), uint8_t **stubs, uint32_t *stub_len);

// An overlay blob copied to base and linked there, stubs after its text; lookup maps outside vrams (0: stub), und is
// o->nund words of scratch. tload_blob_size is base's bytes (0: unresolved); neither touches the run-time tables.
uint32_t tload_blob_size(const XlOut *o, uint32_t (*lookup)(uint32_t), uint32_t *und);
const char *tload_blob(const XlOut *o, uint8_t *base, uint32_t (*lookup)(uint32_t), uint32_t *und);

#endif
