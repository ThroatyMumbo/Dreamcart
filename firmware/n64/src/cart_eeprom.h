#pragma once
#include <stdbool.h>
#include <stdint.h>

// The joybus EEPROM, served through cart_sram: its RAM copy, write-behind and flush are cart_sram_*.

// Core0, before the Maple link starts: brings up the joybus and copies the whole chip into RAM.
void cart_eeprom_init(void);

// Core0, Maple refused: drops the old cart's write state and loads the inserted cart's chip.
void cart_eeprom_reload(void);

// Console 'e': finishes pending writes and dumps the chip itself; if nothing answers, probes the S_DAT line.
void cart_eeprom_diagnose(void);

// Core1, for the Maple cart unit: cart_sram_write, refused unless whole 8-byte blocks.
bool cart_eeprom_write(uint32_t off, uint32_t len, const uint8_t *src);
