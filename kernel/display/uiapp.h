#ifndef UIAPP_H
#define UIAPP_H

#include <stdint.h>
#include "gfx.h"

/*
 * Native desktop apps: drawn in pixels, driven by events.
 *
 * An app is a class (what it is called, how big it starts, its callbacks) and instances of it,
 * one per window. The desktop calls draw() when the window needs painting and hands it the
 * pointer, the keyboard, the wheel and the clock. Nothing here knows which desktop it runs in:
 * tOS's (wm_gfx.c) and ArmtOS's (desktop.c) both implement uihost.h and run the same apps.
 * Apps use integer arithmetic only, because the ARM kernel is built without floating point.
 */

/* keys besides ASCII */
#define UI_KEY_UP     0x11
#define UI_KEY_DOWN   0x12
#define UI_KEY_LEFT   0x13
#define UI_KEY_RIGHT  0x14
#define UI_KEY_DEL    127
#define UI_KEY_ESC    0x1B

typedef struct uiapp uiapp_t;

typedef struct {
    const char *id;          /* what uihost_open() and the launcher call it */
    const char *title;
    const char *icon;        /* key of its icon in the assets */
    int w, h;                /* starting content size */
    int resizable;
    int state_size;          /* bytes of per-window state, zeroed */
    void (*init)(uiapp_t *a, const char *arg);       /* arg: a file to open, or NULL */
    void (*draw)(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h);
    void (*press)(uiapp_t *a, int x, int y, int button);   /* content coordinates */
    void (*drag)(uiapp_t *a, int x, int y);                 /* moved with the button held */
    void (*release)(uiapp_t *a, int x, int y);
    void (*key)(uiapp_t *a, int key);
    void (*wheel)(uiapp_t *a, int delta);
    int  (*tick)(uiapp_t *a);                       /* 1 = needs repainting */
    void (*close)(uiapp_t *a);
} uiapp_class_t;

struct uiapp {
    const uiapp_class_t *cls;
    void *st;                /* the app's own state */
    int w, h;                /* current content size */
    int dirty;
    char title[48];          /* shown in the title bar; starts as the class title */
};

/* every native app, in launcher order */
extern const uiapp_class_t *const uiapp_classes[];
extern const int uiapp_class_count;
const uiapp_class_t *uiapp_find(const char *id);

/* helpers the apps share: the desktop's look, in one place */
#define UI_BG       0x1E1F25
#define UI_CARD     0x2A2B33
#define UI_CARD2    0x33343D
#define UI_LINE     0x3A3C46
#define UI_TEXT     0xF2F3F5
#define UI_DIM      0xA1A4AE
#define UI_ACCENT   0x3B82F6
#define UI_GREEN    0x22C55E
#define UI_RED      0xEF4444
#define UI_ORANGE   0xFF9F0A

void ui_button(gfx_surface_t *s, int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg, int hot);
int  ui_hit(int px, int py, int x, int y, int w, int h);
void ui_num(char *out, int64_t v);            /* appends a decimal number */
void ui_size(char *out, uint32_t bytes);      /* appends "12 KB" style */

#endif
