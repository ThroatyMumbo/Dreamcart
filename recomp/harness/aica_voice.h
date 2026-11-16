#ifndef AICA_VOICE_H
#define AICA_VOICE_H

#include "htypes.h"

// Voices read out of audio command lists, played on AICA channels that each loop a PCM16 ring in sound RAM.
#define AV_CHANNELS 64
#define AV_RING 8192u              // samples per channel ring: 3 lists at pitch 2 with room to spare
#define AV_MIRROR 4096u            // samples of each ring's main-RAM mirror, enough for a list's unflushed writes
#define AV_RING_BASE 0x10000u      // sound RAM offset of channel 0's ring; KOS's ARM idle loop sits at 0
#define AV_LEAD_MS 60u             // queued ahead of the play position: lists arrive late and in bursts
#ifndef AV_CACHE
#define AV_CACHE 1               // 1: resident voices play from the sample cache (docs/n64_sample_cache.md); 0: all stream
#endif
#define AV_RINGS (AV_CACHE ? 40u : 64u)  // rings for streamed voices
#define AV_ARENA_BASE (AV_RING_BASE + AV_RINGS * AV_RING * 2)
#define AV_ARENA_END 0x1FC000u     // the top 16 KB stays free for a reverb DSP ring

// One ADPCM command feeding a voice: its first input frame in RDRAM, and where that frame came from (a ROM offset if
// rom, else the RDRAM address); ~0u when unknown
#define AV_ADPCMS 8u
struct av_adpcm {
    uint32_t ram, book, book_addr;  // book: its codebook's hash, 0 until asked for
    int32_t f;  // its first frame within the note's sample, AV_UNRESOLVED until asked for; -1: not the note's
    uint16_t frames, book_len;
    uint8_t flags;
};
#define AV_UNRESOLVED (-2)

// A run of the list's stream from one ADPCM output, from output index j on (adpcm -1: a CLEAR's zeros)
#define AV_SEGS 12u
struct av_seg { uint16_t at, n, j; int8_t adpcm; };
// A voice's samples this list; sample offsets are into the list's n samples, ~0u if absent
struct av_src {
    uint64_t key;  // keyed (A_INIT) lists: the note's key, with its first frame f0 at output index j0 and its codebook's hash
    uint32_t wrap, end, f0, book, book_addr;  // wrap: first sample after a loop restart; end: first zero past the sample's end
    uint16_t j0, book_len;
    uint8_t ok, keyed, rom, fs, nadpcm, nseg;  // ok: all decoded ADPCM; fs: ADPCM frame bytes, 5 or 9
    uint8_t silent;  // ok, but all CLEAR zeros: past the sample's end
    uint8_t why;  // AA_FG_*: why the stream can't be followed
    struct av_adpcm adpcm[AV_ADPCMS];
    struct av_seg seg[AV_SEGS];
};
// The list's RDRAM, for the cache's own decode: n bytes from ram, a codebook of len bytes into int16s
void aa_bytes(uint32_t ram, uint8_t *out, uint32_t n);
void aa_book(uint32_t addr, uint32_t len, int16_t *out);
// n bytes of a sample from where its frames came from: the ROM (cart reads) if rom, else RDRAM; 0 if unreadable
int aa_frame_src(int rom, uint32_t at, uint8_t *out, uint32_t n);
// Where the PI put RDRAM ram's frame (a ROM offset, *rom set), else ram itself; a codebook's hash, remembered by address
uint32_t aa_frame_at(uint32_t ram, int *rom);
uint32_t aa_book_hash(uint32_t addr, uint32_t len);

void av_list_begin(uint32_t out_hz);
// n samples a voice's resampler consumes from this list, at pitch (Q16.16) over cnt output samples; s is 0 when
// av_plan said the list need not decode them
void av_voice(uint32_t id, int init, const int16_t *s, uint32_t n, uint32_t cnt, uint32_t pitch, struct av_src *src);
// Asked before the list decides what to decode: whether voice id's n samples this list are in the cache already
// (AV_SKIP), must be decoded (AV_DECODE), or must be decoded after av_state rebuilds the ADPCM state skipped lists left
enum { AV_DECODE, AV_SKIP, AV_PATCH };
int av_plan(uint32_t id, int init, uint32_t n, struct av_src *src);
// The 16 samples an ADPCM command whose first input frame is at `frame` (ROM if rom) takes as its state; 0 if unknown
int av_state(uint32_t id, uint32_t frame, int rom, int16_t out[16]);
// Its gains after the envelope, Q15
void av_gain(uint32_t id, uint32_t l, uint32_t r, uint32_t wet);
void av_list_end(void);

// resumed: ids back without A_INIT after a gap; cut: queued samples keyed off
// lost: resident notes the list stopped decoding from ADPCM
struct av_stats {
    uint32_t active, keyons, underruns, overruns, dropped, words, resumed, cut;
    uint32_t res_keyons, res_samples, decoded_frames, patches, lost, outran, late;
    uint32_t why_gap, why_unmapped, why_head, why_book, why_stage, why_foreign;  // why resident notes went on streamed
    uint32_t cold_foreign, cold_unkeyed, cold_refused, cold_dropped;  // why notes streamed from their key-on
    uint32_t streamed_gone, streamed_cold, streamed_silent;  // streamed updates: handed off, with no entry, of those silent
};
extern struct av_stats av_stats;

// Channel events a resident voice queues; the hardware applies each AV_LEAD_MS after it is queued, in order
enum { AV_EV_START = 1, AV_EV_PARAMS, AV_EV_STOP };
struct av_ev { uint32_t ch, kind, sa, lsa, lea, freq_reg, tl, dipan; };

// The hardware below the voice manager: dc/aica_hw.c
void aica_hw_init(void);
// bytes of the ring's main-RAM mirror from sample pos; src, pos and bytes all whole 32-byte blocks, src left alone
// until the next aica_hw_flush returns
void aica_hw_ring_write(uint32_t ch, uint32_t pos, const void *src, uint32_t bytes);
void aica_hw_setup(uint32_t ch, uint32_t ring_off);    // channel keyed off, looping its ring
void aica_hw_params(uint32_t ch, uint32_t freq_reg, uint32_t tl, uint32_t dipan);
void aica_hw_keys(unsigned long long on, unsigned long long off);
uint32_t aica_hw_pos(uint32_t ch);                     // play position in samples
void aica_hw_flush(void);                              // starts the list's ring writes
// Sound RAM at off from src, queued with the ring writes under the same rules
void aica_hw_write(uint32_t off, const void *src, uint32_t bytes);
void aica_hw_read(uint32_t off, void *dst, uint32_t bytes);  // checks only: blocking
void aica_hw_event(const struct av_ev *e);
int aica_hw_idle(uint32_t ch);                         // every event queued for the channel applied
void aica_hw_loop(uint32_t ch, uint32_t lsa, uint32_t lea);  // now, and in a START still queued for the channel
void aica_hw_list_done(void);  // a list ended; with none for a while the output goes silent, as a dry N64 AI does

#endif
