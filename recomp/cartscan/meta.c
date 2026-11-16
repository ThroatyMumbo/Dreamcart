// The XLM2 and XLM3 metas xlate's load_meta reads
#include "boot.h"

#include <string.h>

#include "buf.h"

static void text(Buf *o, const char *s) {
    uint32_t n = (uint32_t)strlen(s);
    static const uint8_t z[4];
    u32(o, n);
    put(o, s, n);
    put(o, z, 4 - n % 4);
}

// The identified functions with a hook policy, in by_address order
void hooks_from_hm(const HmResult *hm, CbHooks *hk) {
    hk->n = 0;
    hk->addr = cs_alloc((hm->naddr + 1) * 4), hk->pol = cs_alloc((hm->naddr + 1) * 2);
    for (uint32_t i = 0; i < hm->naddr; i++)
        for (uint32_t p = 0; p < pol_n; p++)
            if (!strcmp(pol_names[p], sig_names[hm->addr_name[i]])) {
                hk->addr[hk->n] = hm->addr[i], hk->pol[hk->n++] = (uint16_t)p;
                break;
            }
}


#define SEL_ALL (-2)
#define SEL_FIXED (-1)

static int seg_is_ovl(const CsSeg *s) { return CS_OV_IS_KEY(s->vram); }

// The overlay segment holding v (in key space), or -1
static int ovl_of(const CsResult *scan, uint32_t v) {
    for (uint32_t i = 0; i < scan->nseg; i++)
        if (seg_is_ovl(&scan->seg[i]) && v - scan->seg[i].vram < scan->seg[i].size) return (int)i;
    return -1;
}

static int chosen(const CsResult *scan, int sel, uint32_t v) {
    if (sel >= 0 && !seg_is_ovl(&scan->seg[sel])) return v - scan->seg[sel].vram < scan->seg[sel].size;  // a run-time chunk
    return sel == SEL_ALL || ovl_of(scan, v) == (sel == SEL_FIXED ? -1 : sel);
}

static int seg_chosen(const CsResult *scan, int sel, uint32_t i) {
    return sel == SEL_ALL || (sel == SEL_FIXED ? !seg_is_ovl(&scan->seg[i]) : (int)i == sel);
}

static uint8_t *write_meta(const CsResult *scan, const CbHooks *hk, const uint8_t *raw, int sel, cs_page_fn page,
                           void *ctx, uint32_t rom_size, uint32_t *len) {
    Buf o = {0};
    uint32_t nseg = 0, nfunc = 0, ntab = 0, nhook = 0, nrange = 0;
    uint32_t need = 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4;  // sized up front: doubling a 1 MB blob can't fit
    for (uint32_t i = 0; i < scan->nseg; i++)
        if (seg_chosen(scan, sel, i)) nseg++, need += 12 + scan->seg[i].size + 3;
        else if (seg_is_ovl(&scan->seg[i])) nrange++, need += 12;
    for (uint32_t i = 0; i < scan->nfunc; i++)
        if (chosen(scan, sel, scan->func[i].start)) nfunc++, need += 8;
        else need += 5;
    for (uint32_t i = 0; i < scan->ntab; i++)
        if (chosen(scan, sel, scan->tab[i].jr)) ntab++, need += 8 + 4 * scan->tab[i].n;
    for (uint32_t i = 0; i < hk->n; i++)
        if (chosen(scan, sel, hk->addr[i])) nhook++, need += 8 + (uint32_t)strlen(pol_hooks[hk->pol[i]]) + 4;
    o.cap = need;
    o.b = cs_alloc(need);
    put(&o, sel == SEL_ALL ? "XLM2" : "XLM3", 4);
    u32(&o, nseg);
    for (uint32_t i = 0; i < scan->nseg; i++) {
        if (!seg_chosen(scan, sel, i)) continue;
        const CsSeg *s = &scan->seg[i];
        uint32_t lo = s->rom < rom_size ? s->rom : rom_size;
        uint32_t hi = s->size <= rom_size - lo ? lo + s->size : rom_size;
        u32(&o, s->vram);
        u32(&o, s->rom);
        u32(&o, hi - lo);
        put(&o, NULL, hi - lo);
        uint8_t *d = o.b + o.n - (hi - lo);
        for (uint32_t a = lo; a < hi;) {
            const uint32_t *pg = page(ctx, a >> 12);
            if (!(a & 3) && a + 4 <= hi) {
                uint32_t w = pg[(a >> 2) & 1023];
                d[0] = (uint8_t)(w >> 24), d[1] = (uint8_t)(w >> 16), d[2] = (uint8_t)(w >> 8), d[3] = (uint8_t)w;
                d += 4, a += 4;
            } else {
                *d++ = (uint8_t)(pg[(a >> 2) & 1023] >> (8 * (3 - (a & 3))));
                a++;
            }
        }
        static const uint8_t z[4];
        put(&o, z, (4 - (hi - lo) % 4) % 4);
    }
    u32(&o, nfunc);
    for (uint32_t i = 0; i < scan->nfunc; i++)
        if (chosen(scan, sel, scan->func[i].start)) u32(&o, scan->func[i].start), u32(&o, scan->func[i].end);
    for (uint32_t i = 0; i < scan->ntab; i++)
        for (uint32_t j = 0; j < i; j++)
            if (scan->tab[i].jr == scan->tab[j].jr) cs_die("meta: duplicate jr in jumptables");
    u32(&o, ntab);
    for (uint32_t i = 0; i < scan->ntab; i++) {
        if (!chosen(scan, sel, scan->tab[i].jr)) continue;
        u32(&o, scan->tab[i].jr);
        u32(&o, scan->tab[i].n);
        for (uint32_t k = 0; k < scan->tab[i].n; k++) u32(&o, scan->tab[i].targets[k]);
    }
    u32(&o, nhook);
    for (uint32_t i = 0; i < hk->n; i++)
        if (chosen(scan, sel, hk->addr[i])) u32(&o, hk->addr[i]), text(&o, pol_hooks[hk->pol[i]]);
    u32(&o, 0);
    if (sel != SEL_ALL) {  // XLM3: the overlays left out, then the functions left out and whether each is raw
        u32(&o, nrange);
        for (uint32_t i = 0; i < scan->nseg; i++) {
            const CsSeg *s = &scan->seg[i];
            if (seg_chosen(scan, sel, i) || !seg_is_ovl(s)) continue;
            uint32_t lo = s->rom < rom_size ? s->rom : rom_size;
            u32(&o, s->vram), u32(&o, s->rom), u32(&o, s->size <= rom_size - lo ? s->size : rom_size - lo);
        }
        u32(&o, scan->nfunc - nfunc);
        for (uint32_t i = 0; i < scan->nfunc; i++)
            if (!chosen(scan, sel, scan->func[i].start)) u32(&o, scan->func[i].start);
        uint32_t nraw = 0;
        for (uint32_t i = 0; i < scan->nfunc; i++)
            if (!chosen(scan, sel, scan->func[i].start)) {
                uint8_t r = raw ? raw[i] : 0;
                put(&o, &r, 1), nraw++;
            }
        static const uint8_t z[4];
        put(&o, z, (4 - nraw % 4) % 4);
    }
    *len = o.n;
    return o.b;
}

uint8_t *meta_write(const CsResult *scan, const CbHooks *hk, cs_page_fn page, void *ctx, uint32_t rom_size,
                    uint32_t *len) {
    return write_meta(scan, hk, NULL, SEL_ALL, page, ctx, rom_size, len);
}

uint8_t *meta_write_seg(const CsResult *scan, const CbHooks *hk, const uint8_t *raw, int seg, cs_page_fn page,
                        void *ctx, uint32_t rom_size, uint32_t *len) {
    return write_meta(scan, hk, raw, seg < 0 ? SEL_FIXED : seg, page, ctx, rom_size, len);
}

int meta_seg_is_ovl(const CsResult *scan, uint32_t seg) { return seg_is_ovl(&scan->seg[seg]); }
int meta_seg_holds(const CsResult *scan, int seg, uint32_t v) { return chosen(scan, seg < 0 ? SEL_FIXED : seg, v); }
