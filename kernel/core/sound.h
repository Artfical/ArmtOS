#ifndef SOUND_H
#define SOUND_H

/* Short PC-speaker UI cues. Unlike the SB16/AC97 music path, the PC
 * speaker (port 0x61 + PIT channel 2) needs no driver detection — it's
 * present on essentially every real machine and VM, so these always
 * work regardless of what audio hardware (if any) is configured. */

void sound_set_enabled(int on);
int  sound_get_enabled(void);

void sound_click(void); /* button / menu item click */
void sound_open(void);  /* window opened */
void sound_close(void); /* window closed */
void sound_error(void); /* invalid action / error dialog */
void sound_notify(void); /* a notification arrived */

#endif
