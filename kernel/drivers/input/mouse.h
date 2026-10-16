#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

extern int mouse_initialized;
void mouse_init(void);
void mouse_poll(void);
void mouse_set_pixel_mode(int width, int height);   /* coordinates in pixels from now on */
void mouse_get_state(int *x, int *y, uint8_t *buttons);
int mouse_clicked(void);
int mouse_get_click(int *x, int *y);
int mouse_get_rclick(int *x, int *y);
int mouse_get_wheel_delta(void);

#endif
