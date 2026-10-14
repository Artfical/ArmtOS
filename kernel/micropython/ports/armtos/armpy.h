#ifndef ARMPY_H
#define ARMPY_H

/* MicroPython on ArmtOS. */
int  armpy_init(void);
int  armpy_exec(const char *line);     /* runs one statement */
void armpy_repl(void);                 /* the interactive prompt */

#endif
