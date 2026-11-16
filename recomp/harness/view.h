#ifndef VIEW_H
#define VIEW_H

#include "htypes.h"

#define VX8 3
#define VX16 2

extern uint32_t g_view_bias, g_guest_sp;
#define VADDR(v) ((v) + g_view_bias)
#define GUEST_SP g_guest_sp

static inline uint32_t rd32(uint32_t v) { return *(volatile uint32_t *)VADDR(v); }
static inline uint16_t rd16(uint32_t v) { return *(volatile uint16_t *)VADDR(v ^ VX16); }
static inline uint8_t rd8(uint32_t v) { return *(volatile uint8_t *)VADDR(v ^ VX8); }
static inline float rdf(uint32_t v) { union { uint32_t u; float f; } x = { rd32(v) }; return x.f; }
static inline void wr32(uint32_t v, uint32_t w) { *(volatile uint32_t *)VADDR(v) = w; }
static inline void wr16(uint32_t v, uint16_t w) { *(volatile uint16_t *)VADDR(v ^ VX16) = w; }

#define RDRAM_LO 0x80000000u
#define RDRAM_HI (RDRAM_LO + RDRAM_BYTES)
#define GUEST_STACK_TOP 0x803FFF00u
#define GUEST_STACK_LO 0x803FC000u
#define SCRATCH 0x803F0000u

#endif
