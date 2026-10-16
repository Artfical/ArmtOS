#include "../uiapp.h"
#include "../uihost.h"
#include "../imgload.h"
#include "string.h"
#include "memory.h"

/* Image Viewer: PNG and JPEG, fit or zoomed, panned by dragging, the folder's other pictures
 * with the arrow keys, and "Set as Wallpaper". */

#define BAR_H 46

typedef struct {
    char path[224];
    uint8_t *img; uint32_t iw, ih;
    uint8_t *cache; uint32_t cw, ch;      /* the picture at the size it is drawn */
    int zoom;                             /* percent; 0 = fit */
    int panx, pany, drag_x, drag_y, dragging;
    char msg[96];
    ui_filedlg_t dlg;
} viewer_t;

static void drop(viewer_t *v)
{
    if (v->img) free(v->img);
    if (v->cache) free(v->cache);
    v->img = v->cache = 0;
    v->iw = v->ih = v->cw = v->ch = 0;
}

static void open_file(uiapp_t *a, viewer_t *v, const char *path)
{
    drop(v);
    const char *err = 0;
    strncpy(v->path, path, sizeof(v->path) - 1);
    if (img_load(path, &v->img, &v->iw, &v->ih, &err) != 0) {
        strcpy(v->msg, "Cannot show this file: ");
        const char *e = err ? err : "unknown format";
        int o = (int)strlen(v->msg);
        while (*e && o < (int)sizeof(v->msg) - 1) v->msg[o++] = *e++;
        v->msg[o] = 0;
        v->img = 0;
    } else v->msg[0] = 0;
    v->zoom = 0; v->panx = v->pany = 0;
    strcpy(a->title, ui_basename(path));
    a->dirty = 1;
}

static int is_picture(const char *n)
{
    const char *d = 0;
    for (const char *p = n; *p; p++) if (*p == '.') d = p;
    if (!d) return 0;
    char e[8] = "";
    for (int i = 0; i < 6 && d[1 + i]; i++) e[i] = (char)(d[1 + i] >= 'A' && d[1 + i] <= 'Z' ? d[1 + i] + 32 : d[1 + i]);
    return !strcmp(e, "png") || !strcmp(e, "jpg") || !strcmp(e, "jpeg") || !strcmp(e, "jfif");
}

/* the next or previous picture in the same folder */
static void step(uiapp_t *a, viewer_t *v, int dir)
{
    if (!v->path[0]) return;
    char folder[224];
    strcpy(folder, v->path);
    char *slash = 0;
    for (char *p = folder; *p; p++) if (*p == '/') slash = p;
    if (!slash) return;
    if (slash == folder) slash[1] = 0; else *slash = 0;
    static uihost_entry_t e[128];
    int n = uihost_list(folder, e, 128);
    const char *cur = ui_basename(v->path);
    int idx = -1, cnt = 0;
    int pics[128];
    for (int i = 0; i < n; i++) if (!e[i].is_dir && is_picture(e[i].name)) { if (!strcmp(e[i].name, cur)) idx = cnt; pics[cnt++] = i; }
    if (cnt < 2 || idx < 0) return;
    int ni = (idx + dir + cnt) % cnt;
    char p[224];
    ui_path_join(p, folder, e[pics[ni]].name);
    open_file(a, v, p);
}

static void v_init(uiapp_t *a, const char *arg)
{
    viewer_t *v = a->st;
    if (arg) open_file(a, v, arg);
    else ui_filedlg_open(&v->dlg, 0, 0, 0);
}

static void v_close(uiapp_t *a) { drop(a->st); }

static void v_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    viewer_t *v = a->st;
    gfx_fill(s, x, y, w, BAR_H, 0x24252C);
    gfx_hline(s, x, y + BAR_H - 1, w, 0, 120);
    ui_button(s, x + 10, y + 9, 70, 28, "Open", UI_CARD2, UI_TEXT, 0);
    ui_button(s, x + 90, y + 9, 34, 28, "\xE2\x80\x93", UI_CARD2, UI_TEXT, 0);
    ui_button(s, x + 128, y + 9, 50, 28, "Fit", v->zoom == 0 ? UI_ACCENT : UI_CARD2, UI_TEXT, 0);
    ui_button(s, x + 182, y + 9, 34, 28, "+", UI_CARD2, UI_TEXT, 0);
    ui_button(s, x + w - 150, y + 9, 140, 28, "Set as Wallpaper", UI_CARD2, v->img ? UI_TEXT : 0x5C5F6A, 0);
    char info[64] = "";
    if (v->img) { ui_num(info, v->iw); strcat(info, " x "); ui_num(info, v->ih); strcat(info, "   "); ui_num(info, v->zoom ? v->zoom : (int)(v->cw * 100 / v->iw)); strcat(info, "%"); }
    gfx_text(s, &ui_font_small, x + 228, y + 15, info, UI_DIM, 255);

    int cy = y + BAR_H, chh = h - BAR_H;
    /* a quiet checkerboard, so transparent parts of a picture show as such */
    gfx_fill(s, x, cy, w, chh, 0x17181C);
    if (v->img) {
        uint32_t tw, th;
        if (v->zoom == 0) {
            tw = (uint32_t)w - 24; th = v->ih * tw / v->iw;
            if (th > (uint32_t)chh - 24) { th = (uint32_t)chh - 24; tw = v->iw * th / v->ih; }
            if (tw > v->iw && th > v->ih) { tw = v->iw; th = v->ih; }   /* never blow a small picture up to fit */
        } else { tw = v->iw * (uint32_t)v->zoom / 100; th = v->ih * (uint32_t)v->zoom / 100; }
        if (!tw) tw = 1;
        if (!th) th = 1;
        if (tw * th > 16u * 1024 * 1024) { tw = v->cw ? v->cw : 1; th = v->ch ? v->ch : 1; }
        if (!v->cache || v->cw != tw || v->ch != th) {
            if (v->cache) free(v->cache);
            v->cache = (tw == v->iw && th == v->ih) ? 0 : img_scale(v->img, v->iw, v->ih, tw, th);
            v->cw = tw; v->ch = th;
        }
        gfx_image_t im = { (int)tw, (int)th, v->cache ? v->cache : v->img };
        int ix = x + (w - (int)tw) / 2 + v->panx, iy = cy + (chh - (int)th) / 2 + v->pany;
        gfx_surface_t c = *s;
        gfx_clip(&c, x, cy, w, chh);
        for (int yy = 0; yy < (int)th; yy += 12)
            for (int xx = 0; xx < (int)tw; xx += 12)
                gfx_fill(&c, ix + xx, iy + yy, 12, 12, ((xx / 12 + yy / 12) & 1) ? 0x2C2D33 : 0x3A3B42);
        gfx_blit(&c, &im, ix, iy, 255);
    } else if (!v->dlg.active) {
        gfx_text_center(s, &ui_font_regular, x, cy + chh / 2 - 10, w, v->msg[0] ? v->msg : "Open a PNG or JPEG picture", UI_DIM, 255);
    }
    if (v->img && v->msg[0]) gfx_text_center(s, &ui_font_small, x, y + h - 22, w, v->msg, 0x86EFAC, 255);
    ui_filedlg_draw(&v->dlg, s, x, y, w, h);
}

static void v_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    viewer_t *v = a->st;
    a->dirty = 1;
    if (v->dlg.active) { if (ui_filedlg_press(&v->dlg, px, py, a->w, a->h) == 1) open_file(a, v, v->dlg.result); return; }
    if (py < BAR_H) {
        int cur = v->zoom ? v->zoom : (v->iw ? (int)(v->cw * 100 / v->iw) : 100);
        if (ui_hit(px, py, 10, 9, 70, 28)) ui_filedlg_open(&v->dlg, 0, 0, 0);
        else if (ui_hit(px, py, 90, 9, 34, 28)) v->zoom = cur > 20 ? cur * 2 / 3 : 10;
        else if (ui_hit(px, py, 128, 9, 50, 28)) { v->zoom = 0; v->panx = v->pany = 0; }
        else if (ui_hit(px, py, 182, 9, 34, 28)) v->zoom = cur < 800 ? cur * 3 / 2 : 800;
        else if (ui_hit(px, py, a->w - 150, 9, 140, 28) && v->img)
            strcpy(v->msg, uihost_set_wallpaper(v->path) == 0 ? "Wallpaper set" : "Could not set the wallpaper");
        return;
    }
    v->dragging = 1; v->drag_x = px - v->panx; v->drag_y = py - v->pany;
}

static void v_drag(uiapp_t *a, int px, int py)
{
    viewer_t *v = a->st;
    if (!v->dragging || v->zoom == 0) return;
    v->panx = px - v->drag_x; v->pany = py - v->drag_y;
    a->dirty = 1;
}

static void v_release(uiapp_t *a, int x, int y) { (void)x; (void)y; ((viewer_t *)a->st)->dragging = 0; }

static void v_key(uiapp_t *a, int k)
{
    viewer_t *v = a->st;
    a->dirty = 1;
    if (v->dlg.active) { if (ui_filedlg_key(&v->dlg, k) == 1) open_file(a, v, v->dlg.result); return; }
    if (k == UI_KEY_RIGHT || k == ' ') step(a, v, 1);
    else if (k == UI_KEY_LEFT) step(a, v, -1);
    else if (k == '+' || k == '=') v_press(a, 190, 20, 1);
    else if (k == '-') v_press(a, 100, 20, 1);
    else if (k == '0' || k == 'f') { v->zoom = 0; v->panx = v->pany = 0; }
}

static void v_wheel(uiapp_t *a, int d) { v_press(a, d > 0 ? 190 : 100, 20, 1); }

const uiapp_class_t app_viewer = {
    "viewer", "Image Viewer", "viewer", 760, 520, 1, sizeof(viewer_t),
    v_init, v_draw, v_press, v_drag, v_release, v_key, v_wheel, 0, v_close,
};
