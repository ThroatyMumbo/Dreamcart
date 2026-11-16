// Dreamcart 64's restart into its menu: link with dc64_entry.S as the ELF entry and --wrap=arch_real_exit
#ifndef DC64_RESTART_H
#define DC64_RESTART_H

#include <stdbool.h>
#include <stdint.h>

void dc64_restart_arm(void);     // the next arch_abort restarts this ELF instead of returning to dcload
bool dc64_restart_ready(void);   // false if .data outgrew the snapshot, and an armed abort still goes to dcload
uint32_t dc64_boot_count(void);  // restarts since dcload started the ELF
void dc64_restart_note(const char *note);  // a line for the next menu to show; survives the restart
void dc64_take_note(char *out, size_t n);  // that line, or "", and clears it

#endif
