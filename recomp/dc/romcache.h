#ifndef ROMCACHE_H
#define ROMCACHE_H

#include <stdint.h>

typedef int (*romcache_fetch_fn)(uint32_t off, void *dst, uint32_t len);

// 4 KB ROM blocks kept in console RAM in front of a slow cart link; the ROM is read-only, so it never goes stale.
void romcache_init(romcache_fetch_fn fetch);
void romcache_add(void *mem, uint32_t bytes);  // donates RAM to the cache; call any number of times
void romcache_add_heap(void *mem, uint32_t bytes);  // the same for a malloc'd block, which romcache_release can free
uint32_t romcache_release(void);  // frees the newest malloc'd donation, its blocks dropped; its bytes, 0 if none left
uint32_t romcache_bytes(void);
int romcache_read(uint32_t off, void *dst, uint32_t len);

extern struct romcache_stats { uint64_t hit_bytes, fetched_bytes; } romcache_stats;

#endif
