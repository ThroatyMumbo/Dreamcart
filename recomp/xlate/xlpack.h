#ifndef XLPACK_H
#define XLPACK_H

// xlate's output before tload, as one position-independent blob for the reader's SD cache ("XLP1").
// A 128-byte header, then word-aligned sections: text, relocations, function vrams, function offsets, names, overlays.
#include <stdint.h>

#include "xlate.h"

#define XLP_HDR  128u
#define XLP_NSEC 6u

typedef struct {
    uint32_t off, len;
    const void *src;
} XlpOut;

typedef struct {
    uint32_t off, len;
    void *dst;
} XlpIn;

typedef struct {
    uint32_t hdr[XLP_HDR / 4];
    XlpOut sec[XLP_NSEC];
    uint32_t total, crc;    // crc: zlib CRC-32 of the whole blob, header first
    uint32_t *rel, *names;  // the packed copies the sections point into
} XlPack;

uint32_t xl_version(void);  // a CRC of the translator's sources: a translator change is a different key
uint32_t xl_pack_key(uint32_t meta_len);

// 0 when o doesn't fit the format; xl_pack_free either way
int xl_pack(const XlOut *o, uint32_t meta_crc, uint32_t meta_len, XlPack *p);
void xl_pack_free(XlPack *p);

// From a header read off the blob: 0 unless it is this meta's, by this translator. Allocates o's arrays and says
// where each section's bytes go; read them all into place, then xl_unpack_end. Any failure: xl_unpack_drop.
int xl_unpack_begin(const uint32_t *hdr, uint32_t meta_crc, uint32_t meta_len, XlOut *o, XlpIn in[XLP_NSEC]);
int xl_unpack_end(XlOut *o, XlpIn in[XLP_NSEC]);
void xl_unpack_drop(XlOut *o, XlpIn in[XLP_NSEC]);  // frees what begin allocated and zeroes o
uint32_t xl_unpack_total(const XlpIn in[XLP_NSEC]);  // the blob's bytes

// Stored form: the blob and relocation deltas in XLP_BLOCK pieces, each a word (bit 31 raw, the rest its length) and
// its bytes, LZ4 (xlz.h) unless that saves nothing, padded to a word; so a piece grows by at most its 4-byte header
#define XLP_BLOCK   16384u
#define XLP_REC_MAX (XLP_BLOCK + XLP_BLOCK / 255u + 24u)

uint32_t xlp_records(const XlPack *p);   // pieces in p's stored form
uint32_t xlp_store_max(const XlPack *p); // the most its stored form takes
// Piece k into rec, returning its bytes; linked: p's text as tload left it, unlinked on the way (NULL: xlate's own).
// *tcrc accumulates the CRC-32 of the text it emits: over every piece, xlate's text's. scratch: XLP_BLOCK bytes.
uint32_t xlp_record(const XlPack *p, uint32_t k, const uint8_t *linked, uint8_t *rec, uint8_t *scratch,
                    uint16_t *work, uint32_t *tcrc);

typedef struct {
    XlOut *o;
    XlpIn in[XLP_NSEC];
    uint32_t meta_crc, meta_len, pos, have, total, began;
    uint32_t hdr[XLP_HDR / 4];
    uint8_t rec[XLP_REC_MAX] __attribute__((aligned(4)));
    uint8_t blk[XLP_BLOCK] __attribute__((aligned(4)));
} XlpLoad;

// The stored form back into o: begin, feed every stored byte in order, end. 0 from feed or end: not this meta's
// entry, or damaged; then xlp_load_drop
void xlp_load_begin(XlpLoad *l, uint32_t meta_crc, uint32_t meta_len, XlOut *o);
int xlp_load_feed(XlpLoad *l, const uint8_t *b, uint32_t n);
int xlp_load_end(XlpLoad *l);
void xlp_load_drop(XlpLoad *l);

#endif
