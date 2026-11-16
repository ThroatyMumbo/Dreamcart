// Port 0's Controller Pak in a VMS file on the first VMU in a controller: one pak shared by every game, as a real one
// is. The whole file sits in RAM; a background thread writes the blocks the game changed once its writes settle.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arch/irq.h>
#include <arch/timer.h>
#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <dc/vmufs.h>
#include <dc/vmu_pkg.h>
#include <kos/mutex.h>
#include <kos/sem.h>
#include <kos/thread.h>
#include "pif.h"
#include "vmulcd.h"
#include "vmupak.h"

#define NAME "DC64CPAK"
#define HDR_BYTES 0x280u  // the VMS header and one icon
#define FILE_BYTES (HDR_BYTES + PIF_PAK_BYTES)
#define BLOCKS ((FILE_BYTES + 511) / 512)
#define NW ((BLOCKS + 31) / 32)
_Static_assert(BLOCKS == VMUPAK_BLOCKS, "VMUPAK_BLOCKS is the file's size");
#define SETTLE_MS 500   // libultra writes a note as many 32-byte rows plus the inode and note tables
#define RETRY_MS 2000

static maple_device_t *dev;
static uint8_t file[BLOCKS * 512] __attribute__((aligned(32)));
static uint16_t chain[BLOCKS];
static volatile uint32_t dirty[NW];
static volatile uint64_t last_write;
static mutex_t lock = MUTEX_INITIALIZER;
static semaphore_t wake = SEM_INITIALIZER(0);
static volatile int woken;

static maple_device_t *find_vmu(void) {
    for (int p = 0; p < MAPLE_PORT_COUNT; p++) {
        maple_device_t *c = maple_enum_dev(p, 0);
        if (!c || !(c->info.functions & MAPLE_FUNC_CONTROLLER)) continue;
        for (int u = 1; u < MAPLE_UNIT_COUNT; u++) {
            maple_device_t *m = maple_enum_dev(p, u);
            if (m && (m->info.functions & MAPLE_FUNC_MEMCARD)) return m;
        }
    }
    return NULL;
}

// KOS's vmu_pkg_crc, which is static there
static uint16_t vms_crc(const uint8_t *p, uint32_t n) {
    uint16_t c = 0;
    for (uint32_t i = 0; i < n; i++) {
        c ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++) c = c & 0x8000 ? (uint16_t)(c << 1 ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

static int crc_ok(void) {
    vmu_hdr_t *h = (vmu_hdr_t *)file;
    uint16_t want = h->crc;
    h->crc = 0;
    uint16_t got = vms_crc(file, FILE_BYTES);
    h->crc = want;
    return got == want;
}

static void seal(void) {
    vmu_hdr_t *h = (vmu_hdr_t *)file;
    h->crc = 0;
    h->crc = vms_crc(file, FILE_BYTES);
}

// The file's blocks in order into chain: 1 found, 0 absent, -1 a VMU error, -2 a file of that name but not this layout
static int locate(maple_device_t *d) {
    vmu_root_t root;
    if (vmufs_root_read(d, &root) < 0) return -1;
    uint32_t fat_n = (uint32_t)vmufs_fat_blocks(&root) / 2;
    vmu_dir_t *dir = malloc((size_t)vmufs_dir_blocks(&root));
    uint16_t *fat = malloc(fat_n * 2);
    int r = -1;
    if (dir && fat && vmufs_dir_read(d, &root, dir) >= 0 && vmufs_fat_read(d, &root, fat) >= 0) {
        int i = vmufs_dir_find(&root, dir, NAME);
        r = i < 0 ? 0 : dir[i].filesize == BLOCKS && dir[i].hdroff == 0 ? 1 : -2;
        uint16_t b = r == 1 ? dir[i].firstblk : 0;
        for (unsigned k = 0; r == 1 && k < BLOCKS; k++) {
            if (b >= fat_n) r = -2;
            else chain[k] = b, b = fat[b];
        }
        if (r == 1 && b != 0xFFFA) r = -2;
    }
    free(dir), free(fat);
    return r;
}

static void pad(char *d, const char *s, unsigned n, char fill) {
    for (unsigned i = 0; i < n; i++) d[i] = *s ? *s++ : fill;
}

// A gray pak with a dark label, for the BIOS file manager
static void draw_icon(vmu_hdr_t *h, uint8_t *icon) {
    static const uint16_t pal[4] = { 0x0000, 0xF999, 0xF333, 0xFCCC };  // ARGB4444
    memcpy(h->icon_pal, pal, sizeof pal);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
            int c = x < 4 || x > 27 || y < 5 || y > 27 ? 0 : x >= 8 && x <= 23 && y >= 9 && y <= 17 ? 2 : y >= 24 ? 3 : 1;
            icon[y * 16 + x / 2] |= (uint8_t)(c << (x & 1 ? 0 : 4));
        }
}

static int create(void) {
    int free_blocks = vmufs_free_blocks(dev);
    if (free_blocks < (int)BLOCKS) {
        printf("vmupak: VMU full (%d of %u blocks free), no pak\n", free_blocks, (unsigned)BLOCKS);
        return 0;
    }
    memset(file, 0, sizeof file);
    vmu_hdr_t *h = (vmu_hdr_t *)file;
    pad(h->desc_short, "N64 Ctrl Pak", sizeof h->desc_short, ' ');
    pad(h->desc_long, "Dreamcart 64 Controller Pak", sizeof h->desc_long, ' ');
    pad(h->app_id, "DREAMCART64", sizeof h->app_id, 0);
    h->icon_cnt = 1;
    h->data_len = PIF_PAK_BYTES;
    draw_icon(h, file + sizeof *h);
    pif_pak_format(file + HDR_BYTES);
    seal();
    if (vmufs_write(dev, NAME, file, sizeof file, 0) < 0) {
        printf("vmupak: creating " NAME " failed, no pak\n");
        return 0;
    }
    vmufs_mutex_lock();
    int r = locate(dev);
    vmufs_mutex_unlock();
    if (r != 1) printf("vmupak: new " NAME " not found (%d), no pak\n", r);
    else printf("vmupak: created " NAME "\n");
    return r == 1;
}

static int load(void) {
    for (unsigned k = 0; k < BLOCKS; k++)
        if (vmu_block_read(dev, chain[k], file + 512 * k) != MAPLE_EOK) {
            printf("vmupak: block %u read failed, no pak\n", (unsigned)chain[k]);
            return 0;
        }
    vmu_hdr_t *h = (vmu_hdr_t *)file;
    if (h->icon_cnt != 1 || h->eyecatch_type != VMUPKG_EC_NONE || h->data_len != PIF_PAK_BYTES) {
        printf("vmupak: " NAME " has an unknown layout, no pak\n");
        return 0;
    }
    if (!crc_ok()) printf("vmupak: " NAME " CRC mismatch, using it anyway\n");
    return 1;
}

// cart_read flushes the Maple queue whenever the bus is idle, so unlike KOS's vblank pace a VMU still programming
// the last phase gets the next at once and doesn't answer (KOS's vmu_block_write then sleeps and redoes the block)
#define PHASE_GAP_MS 20
#define REPLY_MS 200
#define PHASE_TRIES 3

static maple_frame_t wframe;
static uint32_t wargs[2 + 128 / 4];
static volatile int wresp;  // the reply's response code; 1 while none has come

static void wreply(maple_state_t *st, maple_frame_t *f) {
    (void)st;
    wresp = ((const maple_response_t *)f->recv_buf)->response;
    maple_frame_unlock(f);
}

// The VMU's response code, or MAPLE_RESPONSE_NONE when it doesn't answer
static int xact(uint8_t cmd, int words) {
    if (maple_frame_lock(&wframe) < 0) return MAPLE_RESPONSE_NONE;
    maple_frame_init(&wframe);
    wframe.cmd = cmd;
    wframe.dst_port = dev->port;
    wframe.dst_unit = dev->unit;
    wframe.length = words;
    wframe.send_buf = wargs;
    wframe.callback = wreply;
    wresp = 1;
    maple_queue_frame(&wframe);
    uint64_t t0 = timer_ms_gettime64();
    while (wresp == 1 && timer_ms_gettime64() - t0 < REPLY_MS) thd_sleep(1);
    if (wresp == 1) {
        irq_mask_t s = irq_disable();
        if (wframe.queued) maple_queue_remove(&wframe);
        wframe.state = MAPLE_FRAME_VACANT;
        irq_restore(s);
        return MAPLE_RESPONSE_NONE;
    }
    return wresp;
}

static uint32_t blk_arg(uint16_t blk, int phase) { return (uint32_t)(blk & 0xFF) << 24 | (uint32_t)(blk >> 8) << 16 | phase << 8; }

// A block in its four 128-byte phases, then the sync KOS's vmu_block_write sends
static int block_write(uint16_t blk, const uint8_t *data) {
    for (int phase = 0; phase < 4; phase++) {
        int ok = 0;
        for (int t = 0; !ok && t < PHASE_TRIES; t++) {
            wargs[0] = MAPLE_FUNC_MEMCARD;
            wargs[1] = blk_arg(blk, phase);
            memcpy(wargs + 2, data + 128 * phase, 128);
            ok = xact(MAPLE_COMMAND_BWRITE, 2 + 128 / 4) == MAPLE_RESPONSE_OK;
            thd_sleep(PHASE_GAP_MS);
        }
        if (!ok) return 0;
    }
    wargs[0] = MAPLE_FUNC_MEMCARD;
    wargs[1] = blk_arg(blk, 4);
    return xact(MAPLE_COMMAND_BSYNC, 2) != MAPLE_RESPONSE_NONE;
}

static int any_dirty(void) {
    for (unsigned i = 0; i < NW; i++)
        if (dirty[i]) return 1;
    return 0;
}

static void mark_block(uint32_t k) { dirty[k >> 5] |= 1u << (k & 31); }

static void mark(uint32_t off) {
    irq_mask_t s = irq_disable();
    mark_block(0);
    mark_block((HDR_BYTES + off) / 512);
    mark_block((HDR_BYTES + off + 31) / 512);
    irq_restore(s);
    last_write = timer_ms_gettime64();
    if (!woken) woken = 1, sem_signal(&wake);
}

// Every dirty block, the header with its CRC last; 0 if a write failed, with what is left still dirty
static int write_back(unsigned *written) {
    static uint8_t buf[512] __attribute__((aligned(32)));
    uint32_t todo[NW];
    irq_mask_t s = irq_disable();
    for (unsigned i = 0; i < NW; i++) todo[i] = dirty[i], dirty[i] = 0;
    irq_restore(s);
    seal();
    *written = 0;
    for (int k = BLOCKS - 1; k >= 0; k--) {
        if (!(todo[k >> 5] >> (k & 31) & 1)) continue;
        memcpy(buf, file + 512 * k, 512);
        if (!block_write(chain[k], buf)) {
            s = irq_disable();
            for (int j = k; j >= 0; j--)
                if (todo[j >> 5] >> (j & 31) & 1) mark_block((uint32_t)j);
            irq_restore(s);
            return 0;
        }
        ++*written;
    }
    return 1;
}

static int flush_locked(void) {
    static int failing;
    unsigned n;
    uint64_t t0 = timer_ms_gettime64();
    vmulcd_saving(1);
    int ok = write_back(&n);
    vmulcd_saving(0);
    if (ok && n) printf("vmupak: %u blocks written in %lu ms\n", n, (unsigned long)(timer_ms_gettime64() - t0));
    else if (!failing) printf("vmupak: write failed after %u blocks, retrying\n", n);
    failing = !ok;
    return ok;
}

// Asleep until the game changes the pak, so it never runs during play otherwise
static void *writer(void *arg) {
    (void)arg;
    for (;;) {
        sem_wait(&wake);
        for (;;) {
            uint64_t quiet = timer_ms_gettime64() - last_write;
            if (quiet < SETTLE_MS) {
                thd_sleep((int)(SETTLE_MS - quiet));
                continue;
            }
            woken = 0;  // a write during the flush wakes the writer again
            mutex_lock(&lock);
            int ok = flush_locked();
            mutex_unlock(&lock);
            if (ok) break;
            thd_sleep(RETRY_MS);
        }
    }
    return NULL;
}

int vmupak_short(void) {
    maple_device_t *d = find_vmu();
    if (!d) return -1;
    vmufs_mutex_lock();
    int r = locate(d);
    vmufs_mutex_unlock();
    int n = r == 0 ? vmufs_free_blocks(d) : -1;
    return n >= 0 && n < (int)BLOCKS ? n : -1;
}

void vmupak_open(void) {
    uint64_t t0 = timer_ms_gettime64();
    dev = find_vmu();
    if (!dev) {
        printf("vmupak: no VMU, no pak\n");
        return;
    }
    vmufs_mutex_lock();
    int r = locate(dev);
    vmufs_mutex_unlock();
    if (r == -1) printf("vmupak: VMU %c%d read failed, no pak\n", 'A' + dev->port, dev->unit);
    if (r == -2) printf("vmupak: " NAME " has an unknown layout, no pak\n");
    int ok = r == 0 ? create() : r == 1 && load();
    if (!ok) {
        dev = NULL;
        return;
    }
    pif_on_pak_write = mark;
    pif_attach_pak(file + HDR_BYTES);
    kthread_t *t = thd_create(true, writer, NULL);
    if (t) thd_set_prio(t, PRIO_DEFAULT + 1);  // below the game's thread
    else printf("vmupak: no writer thread\n");
    printf("vmupak: pak on VMU %c%d, %lu ms\n", 'A' + dev->port, dev->unit,
           (unsigned long)(timer_ms_gettime64() - t0));
}

void vmupak_flush(void) {
    if (!dev || !any_dirty()) return;
    mutex_lock(&lock);
    flush_locked();
    mutex_unlock(&lock);
}
