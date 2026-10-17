#ifndef WM_NOTIFY_H
#define WM_NOTIFY_H

#include <stdint.h>
#include "gfx.h"

/*
 * Notifications for the graphical desktop: banners at the top right, Notification Centre (a
 * panel that opens from the clock) keeping what arrived, and Do Not Disturb, which silences
 * the banners and the chime but still keeps everything in the Centre.
 */

/* app: the app's id ("paint", "files"; its dock icon is used when icon is NULL) or a name like
 * "System"; icon: an image name ("doc48_disk", "icon48_paint") or NULL; act1/act2: button
 * labels or NULL. Returns the notification's id (> 0), or -1 when the list is full. */
int  notify_post(const char *app, const char *icon, const char *title, const char *body,
                 const char *act1, const char *act2);
/* 0 still waiting, 1 or 2 that action was clicked, 3 the notification itself was clicked,
 * -1 closed or cleared, -2 no such notification */
int  notify_result(int id);

int  notify_dnd(void);
void notify_set_dnd(int on);
int  notify_center_is_open(void);
void notify_center_toggle(void);

/* the window manager's side: draws on its back buffer, routes clicks, animates */
void notify_draw(gfx_surface_t *s, int sw, int sh, int topbar_h);
int  notify_press(int x, int y, char *open_app, int open_app_len);   /* 1 = the click was ours */
int  notify_tick(void);                                               /* 1 = repaint */

#endif
