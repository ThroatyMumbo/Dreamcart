#ifndef AICA_CACHE_H
#define AICA_CACHE_H

#include "htypes.h"

// Decoded samples in sound RAM, by sample index: position 16 + i holds sample i, after 16 silent ones (an A_INIT
// ADPCM's zero state). Frames are decoded as notes reach them, until the loop or the end (docs/n64_sample_cache.md)
#define AC_ENTRIES 256u  // SM64 and MK64 keep under 32; the rest remember size hints
#define AC_MAX_SAMPLES 65504u  // LEA is 16 bits
#define AC_TAIL 16u            // samples of silence a partial entry loops at the end of its space

enum { AC_IDLE = 1, AC_PART, AC_FINAL, AC_NEVER };
enum { AC_BY_LOOP = 1, AC_BY_END };

struct ac_entry {
    unsigned long long key;
    uint32_t off, cap, len, lsa, lea, lru, hint;  // off, cap: bytes of sound RAM; len, lsa, lea: positions
    uint32_t frames, book, lframe;  // frames decoded so far; the codebook's hash; the loop restart's first frame
    uint32_t at;  // where frame 0 came from: a ROM offset if rom, else an RDRAM address
    int16_t last[16];  // the last frame decoded: the next one's state
    uint16_t refs;
    uint8_t state, rom, how, outgrown;  // rom: keyed by a ROM offset, so it means the same sample on the next boot
};

struct ac_stats { uint32_t entries, finals, parts, bytes, evicted, no_room, outgrown, too_long, grown, reopened; };
extern struct ac_stats ac_stats;
// Entries turned final since the reader last cleared ac_nfinals, for checking against the ROM's own loop points
struct ac_final_rec { uint32_t at, len, lsa, lea, frames; uint8_t how, rom; };
#define AC_FINAL_LOG 64
extern struct ac_final_rec ac_finals[AC_FINAL_LOG];
extern uint32_t ac_nfinals;

void ac_init(uint32_t arena_off, uint32_t arena_end);
struct ac_entry *ac_find(unsigned long long key);
// The entry for key, referenced: an existing partial or final one, or a fresh empty partial one; 0 if none fits
struct ac_entry *ac_open(unsigned long long key, int rom, uint32_t book, uint32_t at);
// One VADPCM frame (9 bytes, or 5 if two_bit) decoded after last, into last, exactly as mupen's alist_adpcm does
void ac_decode_frame(int16_t last[16], const uint8_t *in, int two_bit, const int16_t *book);
void ac_unref(struct ac_entry *e);
uint32_t ac_room(const struct ac_entry *e);  // the samples a partial entry can hold before its silent tail
// A partial entry's space grown in place to hold samples, when the arena after it is free; the silent tail moves
int ac_grow(struct ac_entry *e, uint32_t samples);
// A partial entry becomes playable to the end: [lsa, lea) loops; with no loop it ends at len and silence loops after
void ac_final_loop(struct ac_entry *e, uint32_t lsa, uint32_t lea);
void ac_final_end(struct ac_entry *e, uint32_t len);
// An entry ended by a CLEAR that the game then decodes past (n_audio clears mid-sample too) is partial again: 1
int ac_reopen(struct ac_entry *e);
void ac_outgrown(struct ac_entry *e);  // a note outran its space: dropped once unused, the next one gets twice the space
void ac_tick(void);  // once a list: the clock for least-recently-used eviction
int16_t ac_sample(const struct ac_entry *e, uint32_t pos);  // the sample a play reaches at pos (checks)

#endif
