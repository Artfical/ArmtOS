#ifndef WM_INTERNAL_H
#define WM_INTERNAL_H

/* Shared between wm.c (window bookkeeping, apps, the text-mode desktop) and wm_gfx.c (the
 * graphical desktop). Not for apps: they use wm.h. */

#include "terminal.h"

#define MAX_WINDOWS 6
#define NORMAL_W 60
#define NORMAL_H 16

#define WIN_KIND_TERMINAL 0
#define WIN_KIND_NOTEPAD 1
#define WIN_KIND_CLOCK 2
#define WIN_KIND_ABOUT 3
#define WIN_KIND_DISKMGR 4
#define WIN_KIND_CALCULATOR 5
#define WIN_KIND_FILEMGR 6
#define WIN_KIND_PAINT 7
#define WIN_KIND_VIEWER 8
#define WIN_KIND_TASKMGR 9
#define WIN_KIND_SCRIPT      10
#define WIN_KIND_MEDIAPLAYER 11
#define WIN_KIND_NETMON      12
#define WIN_KIND_SNAKE       13
#define WIN_KIND_2048        14
#define WIN_KIND_PDFVIEWER   15
#define WIN_KIND_STICKYNOTES 16
#define WIN_KIND_DOOM        17


typedef struct {
    term_surface_t surface;
    int open;
    int minimized;
    int maximized;
    int pid;
    int z;
    int kind;
    char title[40];
    char initial_cmd[64];
    int x0, y0, w0, h0;
    int rest_x0, rest_y0, rest_w0, rest_h0;
    int tb_x0, tb_x1;
    /* graphical desktop: the frame in pixels (title bar included) and change tracking */
    int gx, gy, gw, gh;
    int g_rest_x, g_rest_y, g_rest_w, g_rest_h;
    uint32_t g_sum;
    int panel;                       /* WIN_KIND_PANEL: which built-in pixel panel */
    int anim;                        /* frames left of the opening animation */
} window_t;

#define WIN_KIND_PANEL       18   /* drawn by wm_gfx.c itself, no task behind it */

extern window_t windows[MAX_WINDOWS];
extern window_t *wm_focused;
extern int next_z;
extern window_t *content_click_target;
extern int content_click_x, content_click_y;
extern window_t *wheel_target;
extern int pending_wheel_delta;
extern window_t *action_target;
extern int pending_action;
extern int wm_gfx_mode;              /* 1 when the graphical desktop is running */

void wm_focus_window(window_t *w);
void wm_close_window(window_t *w);
void wm_open_terminal(const char *cmd);
int  wm_term_max_view_offset(window_t *w);
void wm_desktop_new_folder(void);
void wm_desktop_new_file(void);
int  wm_find_free_slot(void);

/* wm_gfx.c */
int  wm_gfx_init(void);              /* 0 = graphical desktop available and started */
void wm_gfx_tick(void);
void wm_gfx_window_opened(window_t *w);
void wm_gfx_restore_mode(void);
int  wm_gfx_content_mouse(window_t *w, int *x, int *y);   /* pixel mouse -> content cell */

#endif
