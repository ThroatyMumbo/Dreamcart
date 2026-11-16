// Port 0's Controller Pak, kept in one VMS file (DC64CPAK) on the first VMU in a Dreamcast controller
#ifndef VMUPAK_H
#define VMUPAK_H

#define VMUPAK_BLOCKS 66u  // the VMS header and icon (0x280 bytes) and the 32 KB pak, in 512-byte blocks

// The VMU's free blocks when it has no pak file and too little room to make one; -1 otherwise (or with no VMU)
int vmupak_short(void);
// Loads the pak (or makes a blank one) and plugs it into the PIF; with no usable VMU the game sees no pak
void vmupak_open(void);
// Writes back what the game has changed, now; the background writer otherwise does it once writes settle
void vmupak_flush(void);

#endif
