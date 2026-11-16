#ifndef OVKEY_H
#define OVKEY_H

// The overlay key space cartscan, xlate and the harness share: ROM overlay k's code is keyed at CS_OV_BASE +
// k * CS_OV_SPAN plus its offset; the last slot keys code the game writes into RDRAM (harness/ovl.c)
#define CS_OV_BASE 0x88000000u
#define CS_OV_SPAN 0x00800000u
#define CS_OV_MASK (CS_OV_SPAN - 1u)
#define CS_OV_END 0xA0000000u
#define CS_OV_SLOTS ((CS_OV_END - CS_OV_BASE) / CS_OV_SPAN)  // 48
#define CS_OV_ROM_MAX (CS_OV_SLOTS - 1u)
#define CS_OV_RAM_KEY (CS_OV_BASE + CS_OV_ROM_MAX * CS_OV_SPAN)
#define CS_OV_IS_KEY(v) ((v) >= CS_OV_BASE && (v) < CS_OV_END)

#endif
