#ifndef CARTFLASH_H
#define CARTFLASH_H

// The reader's FlashRAM loaded into the game's model, with a writer thread sending changed pages back; 0 on failure.
int cartflash_open(void);
// Every changed page to the cart now, for an exit or a restart into the menu.
void cartflash_flush(void);

#endif
