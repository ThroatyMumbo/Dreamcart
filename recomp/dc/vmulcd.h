// Every VMU screen: a resting message (the menu's name, then the game's title), or "Saving..." while the pak writes
#ifndef VMULCD_H
#define VMULCD_H

void vmulcd_show(const char *text);  // word-wrapped and centered; kept as the resting message
void vmulcd_saving(int on);          // "Saving..." on; off puts the resting message back

#endif
