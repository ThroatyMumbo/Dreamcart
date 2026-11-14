#pragma once

// Console 'f': prints the FlashRAM id; returns its row in the id table, or -1 if none answered.
int cart_flashram_identify(void);

// Console 'r': the whole 128 KB save as raw binary. Read-only by construction.
void cart_flashram_dump(void);
