#ifndef ARM_COMPAT_SOUND_H
#define ARM_COMPAT_SOUND_H
/* The Pi build has no UI sounds: the desktop's clicks and chimes are silent. */
static inline void sound_click(void) {}
static inline void sound_notify(void) {}
#endif
