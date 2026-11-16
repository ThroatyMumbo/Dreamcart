#ifndef DC64_SFX_H
#define DC64_SFX_H

// The Dreamcart 64 menu's sounds, synthesized into KOS sfx at startup
enum { SFX_MOVE, SFX_SELECT, SFX_DENY, NSFX };
void dc64_sfx_init(void);  // after snd_init
void dc64_sfx(int which);

#endif
