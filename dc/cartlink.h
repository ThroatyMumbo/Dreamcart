// Pipelined, CRC-checked reads from a cart reader (N64 by default; copy of sm64-cart src/cart/cart_link.c).
#ifndef CARTLINK_H
#define CARTLINK_H

#include <stdbool.h>
#include <stdint.h>
#include "../firmware/common/cart_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { CART_LINK_OK, CART_LINK_NO_READER, CART_LINK_NO_CART };
// Finds the reader on any port and learns the cart's byte order; NO_CART leaves the link open for power ops.
int cart_link_probe(void);
bool cart_link_open(void);  // cart_link_probe() == CART_LINK_OK
bool cart_link_is_open(void);
// Cart power through the reader (CART_SYNC_POWER_OFF and _ON): each returns once the reader has carried it out.
bool cart_power_off(void);
int cart_power_on(void);  // finds the reader, powers the cart on if it is off, then probes as cart_link_probe
// ROM bytes in z64 order; false once a fetch fails.
bool cart_read(uint32_t off, void *dst, uint32_t len);
// The cart's save chip: size in bytes (0 if none), whole-word reads, whole-word writes of up to CART_SAVE_WRITE_MAX
// (one EEPROM block, or one FlashRAM page). Both wait while the reader answers NOT_READY.
#define CART_SAVE_WRITE_MAX 128u
uint32_t cart_save_size(void);
uint32_t cart_rom_size(void);   // ROM bytes as the adapter probed them; 0 from older firmware
bool cart_save_read(uint32_t off, void *dst, uint32_t len);
bool cart_save_write(uint32_t off, const void *src, uint32_t len);
// The reader's extras (cart_sync.h): GET_MEDIA_INFO's words after the function code, the spaces, and BLOCK_SYNC
bool cart_media_info(uint32_t *w, unsigned n);
bool cart_space_read(uint32_t space, uint32_t off, void *dst, uint32_t len);
bool cart_space_write(uint32_t space, uint32_t off, const void *src, uint32_t len);
bool cart_sync(uint32_t op, uint32_t a, uint32_t b);
bool cart_rom_is_z64(void);  // the tables are only the ROM's when the bus delivers it in cart order
// zlib CRC-32, valid after cart_link_open.
uint32_t cart_crc32(const void *p, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif
