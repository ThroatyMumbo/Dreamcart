#ifndef CARTSCAN_SIGS_H
#define CARTSCAN_SIGS_H

#include "scan.h"

// libultra functions and variables by Daedalus-style signatures
enum { SR_FUNCTION, SR_VAR_HI, SR_VAR_LO };
typedef struct { uint16_t off, target; uint8_t kind; } SigRef;
typedef struct { uint16_t numops, ref0, nref; uint8_t firstop; uint32_t partial, crc; } SigVar;
typedef struct { uint16_t name, var0, nvar; uint8_t leaf; } SigSym;

// tools/gen_sigdb.py output
extern const uint16_t sig_nnames, sig_nsyms, pol_n;
extern const char *const sig_names[];
extern const int16_t sig_sym_of[];
extern const SigSym sig_syms[];
extern const SigVar sig_vars[];
extern const SigRef sig_refs[];
extern const uint16_t sig_first_start[65], sig_first_list[];
extern const char *const pol_names[], *const pol_hooks[];

typedef struct {
    uint32_t nfunc, nvar, naddr;
    uint16_t *func_name, *var_name, *addr_name;  // indices into sig_names
    uint32_t *func_addr, *var_addr, *addr;       // functions in identification order, variables with both halves
    uint32_t os_thread_tail, os_event_tab, os_rcp_im_table, os_destroy_thread;
    const char *os_missing;                      // the first OS table not identified, or NULL
} HmResult;

HmResult *hm_run(const CsResult *scan, cs_page_fn page, void *ctx, uint32_t rom_size);
// Only segment only's words are candidates (-1: every segment's); the others stay readable for the checks
HmResult *hm_run_seg(const CsResult *scan, int only, cs_page_fn page, void *ctx, uint32_t rom_size);
void hm_free(HmResult *r);
uint32_t cs_crc32(uint32_t crc, const void *buf, size_t len);

#endif
