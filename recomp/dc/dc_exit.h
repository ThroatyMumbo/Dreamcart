#ifndef DC_EXIT_H
#define DC_EXIT_H

#include <stdbool.h>
#include <stdint.h>

// Starts the thread that aborts on A+B+X+Y+Start on any pad, or at DC_DEADLINE_S, whatever main is doing.
bool dc_exit_arm(void);
// Port A as the exit thread last read it: KOS CONT_* buttons, joyx << 16, joyy << 24; refreshed every 4 polls
uint32_t dc_exit_pad(void);
// Port A's analog triggers: rtrig, ltrig << 8
uint32_t dc_exit_triggers(void);
// Called on the exit thread when the deadline passes, before the abort
extern void (*dc_exit_on_deadline)(void);
// Called on the exit thread when the combo is seen, before the abort
extern void (*dc_exit_on_combo)(void);
// Built with -DDC_EXIT_RESTARTS (the menu ELFs), a restart's combo counts only once each port has let it go

#endif
