#ifndef PIF_H
#define PIF_H

#include "htypes.h"

// PIF RAM (64 bytes, N64 byte order). An SI write whose control byte has bit 0 sets the channels up; from then on
// every SI read runs the joybus block in place first (as the PIF does, and libultra writes a read block only once).
void pif_write(uint8_t ram[64]);
void pif_read(uint8_t ram[64]);
// Port 0's standard controller: N64 button bits and stick; ports 1-3 are empty.
void pif_set_pad(uint16_t buttons, int8_t x, int8_t y);
// The cart's EEPROM, kept in RAM: 4 Kbit (512 bytes) or 16 Kbit (2048), as the loaded image's length says.
uint8_t *pif_eeprom(void);
void pif_load_eeprom(const uint8_t *img, uint32_t len);
uint32_t pif_eeprom_bytes(void);
// A cart with no save chip: channel 4 doesn't answer, so osEepromProbe finds none
void pif_no_eeprom(void);
// Each 8-byte block the game writes, after the RAM copy has it (the console passes it on to the real cart)
extern void (*pif_on_eeprom_write)(uint32_t block, const uint8_t data[8]);
// Port 0's Controller Pak: a 32 KB image in N64 byte order, or NULL for none (the default)
#define PIF_PAK_BYTES 0x8000u
void pif_attach_pak(uint8_t *ram);
void pif_pak_format(uint8_t *ram);  // a blank, formatted pak with a fixed serial
// Each 32-byte row the game changes, after the image has it (rewrites of the same bytes don't call it)
extern void (*pif_on_pak_write)(uint32_t off);

#endif
