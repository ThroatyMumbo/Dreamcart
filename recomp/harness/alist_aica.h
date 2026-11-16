#ifndef ALIST_AICA_H
#define ALIST_AICA_H

#include "htypes.h"

struct hle_t;

enum { AA_FG_SEGS = 1, AA_FG_ADPCMS, AA_FG_DEPTH, AA_FG_LOAD, AA_FG_OTHER, AA_FG_NONE, AA_FG_CLEAR, AA_FOREIGN };
struct aa_stats {
    uint32_t lists, cmds, executed, unattributed, kept_resamples;
    uint32_t foreign[AA_FOREIGN];  // voice updates whose stream the cache can't follow, by reason
    uint32_t adpcm_keyons;  // notes keyed on by an A_INIT ADPCM through a resampler without A_INIT
    uint32_t mixed;         // voice sides whose sink is a plain mixer into the dry left or right
};
extern struct aa_stats aa_stats;

enum { AA_ABI1, AA_NEAD, AA_NAUDIO };  // the common audio ABI, MK64's nead, n_aspMain's naudio

// Runs the command list in DMEM's OSTask, as alist_decode left it in cmds[], on hle, in the given ABI, for an AI at hz:
// only what feeds the voices runs, and the voices go to aica_voice.h
void alist_aica_run(struct hle_t *hle, int abi, uint32_t hz);

#endif
