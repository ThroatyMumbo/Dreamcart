#include "xcache.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/sync.h"
#include "crc32.h"
#include "psram.h"
#include "sd.h"

#define SB_MAGIC     0x31435844u    // "DXC1"
#define REC_MAGIC    0x31524358u    // "XCR1"
#define LAYOUT       1u
#define IDX_LBA      1u
#define IDX_RECS     1024u
#define REC_BYTES    128u
#define RECS_SECTOR  (512u / REC_BYTES)
#define IDX_SECTORS  (IDX_RECS / RECS_SECTOR)
#define TEST_LBA     (IDX_LBA + IDX_SECTORS)
#define TEST_SECTORS 2048u
#define DATA_LBA     (TEST_LBA + TEST_SECTORS)
#define MIN_DATA     2048u          // sectors; a card with less room is not used
#define TURN         16u            // sectors per main-loop turn

typedef struct {
    uint32_t magic, layout, sectors, idx_lba, idx_recs, data_lba, crc;
} super_t;

typedef struct {
    uint32_t magic, seq, rom_size, key_a, key_b, lba, len, crc;
    uint8_t id[64];
    uint32_t pad[7];
    uint32_t self_crc;
} rec_t;
_Static_assert(sizeof(rec_t) == REC_BYTES, "rec_t is one index slot");

enum { JOB_NONE, JOB_LOAD, JOB_WRITE };

static rec_t *const g_idx = (rec_t *)(PSRAM_BASE + PSRAM_XC_INDEX);
static uint8_t *const g_stage = (uint8_t *)(PSRAM_BASE + PSRAM_XC_STAGE);
static uint32_t g_bounce[TURN * 128];

static volatile uint32_t g_state = XC_NONE, g_len, g_loaded, g_job;
static volatile uint32_t g_req, g_req_a, g_req_b;
static volatile bool g_off;

static bool g_mounted, g_tried;
static uint32_t g_sectors, g_seq, g_head, g_cap;
static uint8_t g_cart[64];
static uint32_t g_cart_size;

static bool g_keyed;                // a lookup named the key a commit stores under
static uint32_t g_key_a, g_key_b, g_key_size;
static uint8_t g_key_id[64];

static uint32_t g_slot, g_lba, g_pos, g_crc, g_want_crc;    // the job in progress

static uint32_t sectors_of(uint32_t bytes) { return (bytes + 511u) / 512u; }

static bool psram_ok(void) { return psram_size() >= PSRAM_XC_STAGE + (1u << 20); }

static uint32_t capacity(void) {
    if (!g_mounted) return 0;
    uint64_t room = (uint64_t)(g_sectors - DATA_LBA) * 512u, stage = psram_size() - PSRAM_XC_STAGE;
    if (room > stage) room = stage;
    if (g_cap && room > g_cap) room = g_cap;
    return (uint32_t)room;
}

static bool rec_valid(const rec_t *r) {
    return r->magic == REC_MAGIC && r->len && r->lba >= DATA_LBA && r->lba + sectors_of(r->len) <= g_sectors &&
           r->self_crc == crc32_update(0, r, offsetof(rec_t, self_crc));
}

static void sd_lost(void) {
    g_mounted = g_tried = false;
    g_job = JOB_NONE;
}

static bool idx_flush(uint32_t slot) {
    uint32_t s = slot / RECS_SECTOR;
    memcpy(g_bounce, (const uint8_t *)g_idx + s * 512u, 512);
    if (sd_write(IDX_LBA + s, 1, (const uint8_t *)g_bounce)) return true;
    sd_lost();
    return false;
}

static bool idx_drop(uint32_t slot) {
    memset(&g_idx[slot], 0, sizeof(rec_t));
    return idx_flush(slot);
}

static bool format(void) {
    memset(g_bounce, 0, sizeof g_bounce);
    super_t *sb = (super_t *)g_bounce;
    *sb = (super_t){SB_MAGIC, LAYOUT, g_sectors, IDX_LBA, IDX_RECS, DATA_LBA, 0};
    sb->crc = crc32_update(0, sb, offsetof(super_t, crc));
    if (!sd_write(0, 1, (const uint8_t *)g_bounce)) return false;
    memset(g_bounce, 0, sizeof g_bounce);
    for (uint32_t s = 0; s < IDX_SECTORS; s += TURN)
        if (!sd_write(IDX_LBA + s, TURN, (const uint8_t *)g_bounce)) return false;
    memset(g_idx, 0, IDX_RECS * REC_BYTES);
    g_seq = 1, g_head = DATA_LBA;
    printf("xcache: formatted the card\n");
    return true;
}

static bool mount(void) {
    if (!sd_init(SD_MAX_HZ, false)) return false;
    g_sectors = sd_card()->sectors;
    if (g_sectors < DATA_LBA + MIN_DATA || !sd_read(0, 1, (uint8_t *)g_bounce)) return false;
    const super_t *sb = (const super_t *)g_bounce;
    if (sb->magic != SB_MAGIC || sb->layout != LAYOUT || sb->sectors != g_sectors ||
        sb->crc != crc32_update(0, sb, offsetof(super_t, crc)))
        return format();
    for (uint32_t s = 0; s < IDX_SECTORS; s += TURN) {
        if (!sd_read(IDX_LBA + s, TURN, (uint8_t *)g_bounce)) return false;
        memcpy((uint8_t *)g_idx + s * 512u, g_bounce, TURN * 512u);
    }
    uint32_t top = 0;
    g_seq = 1, g_head = DATA_LBA;
    for (uint32_t i = 0; i < IDX_RECS; i++) {
        rec_t *r = &g_idx[i];
        if (!rec_valid(r)) {
            memset(r, 0, sizeof *r);
            continue;
        }
        if (r->seq >= top) top = r->seq, g_seq = r->seq + 1, g_head = r->lba + sectors_of(r->len);
    }
    return true;
}

static bool ensure_mounted(void) {
    if (g_off || !psram_ok()) return false;
    if (!sd_present()) {
        g_mounted = g_tried = false;
        return false;
    }
    if (g_mounted) return true;
    if (g_tried) return false;
    g_tried = true;
    return g_mounted = mount();
}

static bool same_key(const rec_t *r) {
    return r->magic == REC_MAGIC && r->rom_size == g_key_size && r->key_a == g_key_a && r->key_b == g_key_b &&
           !memcmp(r->id, g_key_id, 64);
}

static void lookup(uint32_t a, uint32_t b) {
    g_keyed = true;
    g_key_a = a, g_key_b = b, g_key_size = g_cart_size;
    memcpy(g_key_id, g_cart, 64);
    int best = -1;
    for (uint32_t i = 0; i < IDX_RECS; i++)
        if (same_key(&g_idx[i]) && (best < 0 || g_idx[i].seq > g_idx[best].seq)) best = (int)i;
    if (best < 0) {
        g_state = XC_MISS;
        return;
    }
    g_slot = (uint32_t)best, g_lba = g_idx[best].lba, g_want_crc = g_idx[best].crc;
    g_pos = 0, g_crc = 0, g_loaded = 0, g_len = g_idx[best].len;
    g_job = JOB_LOAD;
    __dmb();
    g_state = XC_HIT;
}

static void load_step(void) {
    uint32_t left = g_len - g_loaded, n = sectors_of(left) < TURN ? sectors_of(left) : TURN;
    uint32_t bytes = left < n * 512u ? left : n * 512u;
    if (!sd_read(g_lba + g_pos, n, (uint8_t *)g_bounce)) {
        sd_lost();
        g_state = XC_FAILED;
        return;
    }
    memcpy(g_stage + g_loaded, g_bounce, bytes);
    g_crc = crc32_update(g_crc, g_bounce, bytes);
    g_pos += n;
    __dmb();
    g_loaded += bytes;
    if (g_loaded < g_len) return;
    g_job = JOB_NONE;
    if (g_crc != g_want_crc) {
        printf("xcache: entry at %lu fails its CRC, dropped\n", (unsigned long)g_lba);
        idx_drop(g_slot);
        g_state = XC_FAILED;
    }
}

static void reserve(uint32_t len) {
    g_job = JOB_NONE;
    if (!g_keyed) { g_state = XC_FAILED; return; }
    if (!len || (len & 3) || len > capacity()) { g_state = XC_FULL; return; }
    g_len = len;
    __dmb();
    g_state = XC_RECV;
}

static bool overlaps(const rec_t *r, uint32_t lba, uint32_t n) {
    return r->magic == REC_MAGIC && r->lba < lba + n && lba < r->lba + sectors_of(r->len);
}

static void commit(uint32_t crc) {
    uint32_t n = sectors_of(g_len);
    if (g_head < DATA_LBA || g_head + n > g_sectors) g_head = DATA_LBA;
    g_lba = g_head;
    for (uint32_t i = 0; i < IDX_RECS; i++)
        if (overlaps(&g_idx[i], g_lba, n) && !idx_drop(i)) { g_state = XC_FAILED; return; }
    g_slot = IDX_RECS;
    for (uint32_t i = 0; i < IDX_RECS && g_slot == IDX_RECS; i++)
        if (g_idx[i].magic != REC_MAGIC) g_slot = i;
    if (g_slot == IDX_RECS) {                   // every slot holds an entry: the oldest goes
        g_slot = 0;
        for (uint32_t i = 1; i < IDX_RECS; i++)
            if (g_idx[i].seq < g_idx[g_slot].seq) g_slot = i;
    }
    g_pos = 0, g_crc = 0, g_want_crc = crc;
    g_job = JOB_WRITE;
}

static void write_step(void) {
    uint32_t total = sectors_of(g_len), done = g_pos * 512u;
    uint32_t n = total - g_pos < TURN ? total - g_pos : TURN, bytes = g_len - done < n * 512u ? g_len - done : n * 512u;
    memcpy(g_bounce, g_stage + done, bytes);
    memset((uint8_t *)g_bounce + bytes, 0, n * 512u - bytes);
    g_crc = crc32_update(g_crc, g_bounce, bytes);
    if (!sd_write(g_lba + g_pos, n, (const uint8_t *)g_bounce)) {
        sd_lost();
        g_state = XC_FAILED;
        return;
    }
    g_pos += n;
    if (g_pos < total) return;
    g_job = JOB_NONE;
    if (g_crc != g_want_crc) { g_state = XC_FAILED; return; }
    rec_t *r = &g_idx[g_slot];
    memset(r, 0, sizeof *r);
    *r = (rec_t){REC_MAGIC, g_seq, g_key_size, g_key_a, g_key_b, g_lba, g_len, g_crc, {0}, {0}, 0};
    memcpy(r->id, g_key_id, 64);
    r->self_crc = crc32_update(0, r, offsetof(rec_t, self_crc));
    if (!idx_flush(g_slot)) { g_state = XC_FAILED; return; }
    for (uint32_t i = 0; i < IDX_RECS; i++)
        if (i != g_slot && same_key(&g_idx[i])) idx_drop(i);
    g_seq++, g_head = g_lba + total;
    g_state = XC_STORED;
}

void xcache_init(void) {
    g_state = ensure_mounted() ? XC_IDLE : XC_NONE;
    if (!psram_ok()) printf("xcache: off, no PSRAM to stage entries in\n");
    else if (!g_mounted) printf("xcache: off, %s\n", sd_present() ? "the SD card did not come up" : "no SD card");
    else printf("xcache: %lu MB card, up to %lu KB an entry\n", (unsigned long)(g_sectors / 2048u),
                (unsigned long)(capacity() >> 10));
}

void xcache_set_cart(const uint8_t id[64], uint32_t size) {
    memcpy(g_cart, id, 64);
    g_cart_size = size;
}

void xcache_service(void) {
    if (g_req) {
        uint32_t op = g_req, a = g_req_a, b = g_req_b;
        __dmb();
        g_req = 0;
        if (op == XC_SYNC_COMMIT) g_len = a, commit(b);  // a compressed entry ends short of what it reserved
        else {
            g_job = JOB_NONE;
            if (!ensure_mounted()) g_state = XC_NONE;
            else if (op == XC_SYNC_LOOKUP) lookup(a, b);
            else reserve(a);
        }
    }
    if (g_job == JOB_LOAD) load_step();
    else if (g_job == JOB_WRITE) write_step();
}

bool __not_in_flash_func(xcache_sync)(uint32_t op, uint32_t a, uint32_t b) {
    if (g_req || g_job == JOB_WRITE) return false;
    if (op == XC_SYNC_COMMIT && (g_state != XC_RECV || !a || a > g_len || (a & 3))) return false;
    if (op != XC_SYNC_LOOKUP && op != XC_SYNC_RESERVE && op != XC_SYNC_COMMIT) return false;
    g_req_a = a, g_req_b = b;
    g_state = XC_BUSY;
    __dmb();
    g_req = op;
    return true;
}

uint32_t __not_in_flash_func(xcache_size)(void) { return g_state == XC_HIT ? g_len : 0; }

bool __not_in_flash_func(xcache_read)(uint32_t off, uint32_t len, uint8_t *dst) {
    if (g_state != XC_HIT || off + len > g_loaded || !psram_enter()) return false;
    memcpy(dst, g_stage + off, len);
    psram_leave();
    return true;
}

bool __not_in_flash_func(xcache_write)(uint32_t off, uint32_t len, const uint8_t *src) {
    if (g_state != XC_RECV || off + len > g_len || !psram_enter()) return false;
    memcpy(g_stage + off, src, len);
    psram_leave();
    return true;
}

void __not_in_flash_func(xcache_info)(uint32_t *state, uint32_t *bytes) {
    uint32_t s = g_off ? XC_NONE : g_state;
    *state = s;
    *bytes = s == XC_HIT ? g_len : capacity();
}

static const char *const state_names[] = {"none", "idle", "busy", "hit", "miss", "receiving", "stored", "failed", "full"};

static void status(void) {
    if (!g_mounted) {
        printf("xcache %s, %s\n", g_off ? "off by command" : "not mounted",
               !psram_ok() ? "no PSRAM" : sd_present() ? "card in the slot" : "no card");
        return;
    }
    uint32_t n = 0, used = 0;
    for (uint32_t i = 0; i < IDX_RECS; i++)
        if (g_idx[i].magic == REC_MAGIC) n++, used += sectors_of(g_idx[i].len);
    printf("xcache %s%s, %lu MB card, %lu entries in %lu MB, head %lu, seq %lu, up to %lu KB an entry%s\n",
           state_names[g_state], g_off ? " (off)" : "", (unsigned long)(g_sectors / 2048u), (unsigned long)n,
           (unsigned long)(used / 2048u), (unsigned long)g_head, (unsigned long)g_seq, (unsigned long)(capacity() >> 10),
           g_cap ? " (capped)" : "");
    for (uint32_t i = 0; i < IDX_RECS; i++) {
        const rec_t *r = &g_idx[i];
        if (r->magic != REC_MAGIC) continue;
        char title[21];
        for (int k = 0; k < 20; k++) title[k] = r->id[0x20 + k] >= 0x20 && r->id[0x20 + k] < 0x7F ? (char)r->id[0x20 + k] : ' ';
        title[20] = 0;
        printf("  %4lu  \"%s\" key %08lX %08lX  %7lu bytes at %lu, seq %lu\n", (unsigned long)i, title,
               (unsigned long)r->key_a, (unsigned long)r->key_b, (unsigned long)r->len, (unsigned long)r->lba,
               (unsigned long)r->seq);
    }
}

// Writes a pattern over the scratch sectors, reads it back, and reports both rates
static void test(void) {
    g_tried = false;
    if (!ensure_mounted()) { printf("xcache: no card to test\n"); return; }
    uint32_t bad = 0, t0 = time_us_32();
    for (uint32_t s = 0; s < TEST_SECTORS; s += TURN) {
        for (uint32_t i = 0; i < TURN * 128; i++) g_bounce[i] = (s * 128 + i) * 0x9E3779B9u;
        if (!sd_write(TEST_LBA + s, TURN, (const uint8_t *)g_bounce)) {
            printf("xcache: write failed at %lu, fail %04lX resp %08lX\n", (unsigned long)(TEST_LBA + s),
                   (unsigned long)sd_stats.fail, (unsigned long)sd_stats.resp);
            sd_lost();
            return;
        }
    }
    uint32_t t1 = time_us_32();
    for (uint32_t s = 0; s < TEST_SECTORS; s += TURN) {
        if (!sd_read(TEST_LBA + s, TURN, (uint8_t *)g_bounce)) {
            printf("xcache: read failed at %lu\n", (unsigned long)(TEST_LBA + s));
            sd_lost();
            return;
        }
        for (uint32_t i = 0; i < TURN * 128; i++) bad += g_bounce[i] != (s * 128 + i) * 0x9E3779B9u;
    }
    uint32_t t2 = time_us_32(), kb = TEST_SECTORS / 2;
    printf("xcache: %lu KB written at %lu KB/s, read back at %lu KB/s, %lu bad words\n", (unsigned long)kb,
           (unsigned long)(kb * 1000000ull / (t1 - t0 + 1)), (unsigned long)(kb * 1000000ull / (t2 - t1 + 1)),
           (unsigned long)bad);
}

static void cmd_xcache(const char *arg) {
    if (!strncmp(arg, "off", 3)) {
        g_off = true;
        g_job = JOB_NONE;
        printf("xcache off: the console sees no card\n");
    } else if (!strncmp(arg, "on", 2)) {
        g_off = false, g_tried = false, g_cap = 0;
        g_state = ensure_mounted() ? XC_IDLE : XC_NONE;
        status();
    } else if (!strncmp(arg, "cap", 3)) {
        g_cap = strtoul(arg + 3, NULL, 0) << 10;
        status();
    } else if (!strncmp(arg, "fmt", 3)) {
        g_tried = false;
        if (ensure_mounted() && format()) g_state = XC_IDLE;
        status();
    } else if (!strncmp(arg, "test", 4)) {
        test();
    } else {
        status();
    }
}

const console_cmd_t xcache_commands[] = {
    { 'n', "n [off|on|cap kb|fmt|test] SD translation cache: status, hide it, cap an entry, wipe, write test",
      cmd_xcache },
};

const unsigned xcache_ncommands = sizeof xcache_commands / sizeof xcache_commands[0];
