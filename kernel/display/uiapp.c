#include "uiapp.h"
#include "string.h"

extern const uiapp_class_t app_calculator, app_clock, app_2048, app_notepad, app_files, app_viewer,
                           app_activity, app_network, app_paint, app_notes;

const uiapp_class_t *const uiapp_classes[] = {
    &app_files, &app_notepad, &app_calculator, &app_paint, &app_viewer, &app_activity, &app_network,
    &app_clock, &app_notes, &app_2048,
};
const int uiapp_class_count = (int)(sizeof(uiapp_classes) / sizeof(uiapp_classes[0]));

const uiapp_class_t *uiapp_find(const char *id)
{
    for (int i = 0; i < uiapp_class_count; i++)
        if (!strcmp(uiapp_classes[i]->id, id)) return uiapp_classes[i];
    return 0;
}

void ui_button(gfx_surface_t *s, int x, int y, int w, int h, const char *label, uint32_t bg, uint32_t fg, int hot)
{
    gfx_rrect(s, x, y, w, h, h / 4 > 9 ? 9 : h / 4, hot ? gfx_mix(bg, 0xFFFFFF, 50) : bg, 255);
    gfx_text_center(s, &ui_font_medium, x, y + (h - 17) / 2, w, label, fg, 255);
}

int ui_hit(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }

void ui_num(char *out, int64_t v)
{
    char t[24];
    int n = 0, o = (int)strlen(out);
    if (v < 0) { out[o++] = '-'; v = -v; }
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) out[o++] = t[--n];
    out[o] = 0;
}

void ui_size(char *out, uint32_t b)
{
    if (b < 1024) { ui_num(out, b); strcat(out, " B"); }
    else if (b < 1024 * 1024) { ui_num(out, (b + 512) / 1024); strcat(out, " KB"); }
    else {
        ui_num(out, b / (1024 * 1024));
        uint32_t frac = (b % (1024 * 1024)) * 10 / (1024 * 1024);
        strcat(out, ".");
        ui_num(out, frac);
        strcat(out, " MB");
    }
}

/* ---- paths ---- */

void ui_path_join(char *out, const char *dir, const char *name)
{
    strcpy(out, dir);
    int n = (int)strlen(out);
    if (n == 0 || out[n - 1] != '/') strcat(out, "/");
    strcat(out, name);
}

const char *ui_basename(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

/* ---- the file chooser ---- */

static void dlg_load(ui_filedlg_t *d)
{
    d->n = uihost_list(d->dir, d->ents, 96);
    if (d->n < 0) d->n = 0;
    /* folders first, then files, each alphabetically */
    for (int i = 1; i < d->n; i++) {
        uihost_entry_t t = d->ents[i];
        int j = i - 1;
        while (j >= 0 && (d->ents[j].is_dir < t.is_dir ||
               (d->ents[j].is_dir == t.is_dir && strcmp(d->ents[j].name, t.name) > 0))) {
            d->ents[j + 1] = d->ents[j];
            j--;
        }
        d->ents[j + 1] = t;
    }
    d->scroll = 0;
    d->sel = -1;
}

void ui_filedlg_open(ui_filedlg_t *d, int saving, const char *start_dir, const char *name)
{
    memset(d, 0, sizeof(*d));
    d->active = 1;
    d->saving = saving;
    strncpy(d->dir, start_dir && start_dir[0] ? start_dir : uihost_home(), sizeof(d->dir) - 1);
    if (name) strncpy(d->name, name, sizeof(d->name) - 1);
    dlg_load(d);
}

#define DLG_W 460
#define DLG_H 380
#define DLG_ROW 26

static void dlg_frame(int w, int h, int *dx, int *dy)
{
    *dx = (w - DLG_W) / 2; *dy = (h - DLG_H) / 2;
    if (*dx < 4) *dx = 4;
    if (*dy < 4) *dy = 4;
}

void ui_filedlg_draw(ui_filedlg_t *d, gfx_surface_t *s, int x, int y, int w, int h)
{
    if (!d->active) return;
    gfx_fill_alpha(s, x, y, w, h, 0x000000, 120);
    int dx, dy;
    dlg_frame(w, h, &dx, &dy);
    dx += x; dy += y;
    gfx_shadow(s, dx, dy, DLG_W, DLG_H, 220);
    gfx_rrect(s, dx, dy, DLG_W, DLG_H, 12, 0x26272E, 255);
    gfx_text(s, &ui_font_medium, dx + 18, dy + 14, d->saving ? "Save As" : "Open", UI_TEXT, 255);
    /* the folder, with an "up" button */
    ui_button(s, dx + 18, dy + 42, 34, 26, "\xE2\x86\x90", UI_CARD2, UI_TEXT, 0);
    gfx_surface_t t = *s;
    gfx_clip(&t, dx + 60, dy + 42, DLG_W - 80, 26);
    gfx_text(&t, &ui_font_regular, dx + 62, dy + 46, d->dir, UI_DIM, 255);
    /* the listing */
    int ly = dy + 78, rows = (DLG_H - 78 - 96) / DLG_ROW;
    gfx_rrect(s, dx + 18, ly, DLG_W - 36, rows * DLG_ROW + 8, 8, 0x1C1D22, 255);
    for (int i = 0; i < rows && d->scroll + i < d->n; i++) {
        uihost_entry_t *e = &d->ents[d->scroll + i];
        int ry = ly + 4 + i * DLG_ROW;
        if (d->scroll + i == d->sel) gfx_rrect(s, dx + 22, ry, DLG_W - 44, DLG_ROW, 6, UI_ACCENT, 255);
        gfx_blit(s, gfx_image(e->is_dir ? "icon20_files" : "icon20_notepad"), dx + 28, ry + 3, 255);
        gfx_text(s, &ui_font_regular, dx + 56, ry + 4, e->name, UI_TEXT, 255);
        if (!e->is_dir) {
            char sz[16] = "";
            ui_size(sz, e->size);
            gfx_text(s, &ui_font_small, dx + DLG_W - 40 - gfx_text_width(&ui_font_small, sz), ry + 6, sz, UI_DIM, 255);
        }
    }
    if (!d->n) gfx_text(s, &ui_font_regular, dx + 32, ly + 12, "(empty folder)", UI_DIM, 255);
    /* the name field and buttons */
    int fy = dy + DLG_H - 84;
    gfx_text(s, &ui_font_small, dx + 18, fy + 6, "Name", UI_DIM, 255);
    gfx_rrect(s, dx + 64, fy, DLG_W - 82, 30, 7, 0x1C1D22, 255);
    gfx_rrect_outline(s, dx + 64, fy, DLG_W - 82, 30, 7, UI_ACCENT, 200);
    int tw = gfx_text(s, &ui_font_regular, dx + 74, fy + 6, d->name, UI_TEXT, 255);
    gfx_fill(s, dx + 75 + tw, fy + 6, 2, 18, UI_ACCENT);
    ui_button(s, dx + DLG_W - 220, dy + DLG_H - 44, 96, 30, "Cancel", UI_CARD2, UI_TEXT, 0);
    ui_button(s, dx + DLG_W - 114, dy + DLG_H - 44, 96, 30, d->saving ? "Save" : "Open", UI_ACCENT, 0xFFFFFF, 0);
}

static void dlg_up(ui_filedlg_t *d)
{
    int n = (int)strlen(d->dir);
    while (n > 1 && d->dir[n - 1] == '/') d->dir[--n] = 0;
    while (n > 1 && d->dir[n - 1] != '/') d->dir[--n] = 0;
    if (n > 1) d->dir[n - 1] = 0;
    if (!d->dir[0]) strcpy(d->dir, "/");
    dlg_load(d);
}

static int dlg_confirm(ui_filedlg_t *d)
{
    if (!d->name[0]) return 0;
    if (d->name[0] == '/') strncpy(d->result, d->name, sizeof(d->result) - 1);
    else ui_path_join(d->result, d->dir, d->name);
    d->active = 0;
    return 1;
}

int ui_filedlg_press(ui_filedlg_t *d, int px, int py, int w, int h)
{
    if (!d->active) return 0;
    int dx, dy;
    dlg_frame(w, h, &dx, &dy);
    px -= dx; py -= dy;
    if (ui_hit(px, py, 18, 42, 34, 26)) { dlg_up(d); return 0; }
    if (ui_hit(px, py, DLG_W - 220, DLG_H - 44, 96, 30)) { d->active = 0; return -1; }
    if (ui_hit(px, py, DLG_W - 114, DLG_H - 44, 96, 30)) return dlg_confirm(d);
    int ly = 78, rows = (DLG_H - 78 - 96) / DLG_ROW;
    if (ui_hit(px, py, 18, ly, DLG_W - 36, rows * DLG_ROW + 8)) {
        int i = d->scroll + (py - ly - 4) / DLG_ROW;
        if (i < 0 || i >= d->n) return 0;
        uint32_t now = uihost_ms();
        int dbl = (i == d->sel && now - d->last_click_ms < 450);
        d->last_click_ms = now;
        d->sel = i;
        if (d->ents[i].is_dir) {
            if (dbl) { char nd[160]; ui_path_join(nd, d->dir, d->ents[i].name); strncpy(d->dir, nd, sizeof(d->dir) - 1); dlg_load(d); }
        } else {
            strncpy(d->name, d->ents[i].name, sizeof(d->name) - 1);
            if (dbl) return dlg_confirm(d);
        }
    }
    return 0;
}

int ui_filedlg_key(ui_filedlg_t *d, int k)
{
    if (!d->active) return 0;
    int n = (int)strlen(d->name);
    if (k == UI_KEY_ESC) { d->active = 0; return -1; }
    if (k == '\n') return dlg_confirm(d);
    if (k == '\b') { if (n) d->name[n - 1] = 0; return 0; }
    if (k == UI_KEY_DOWN && d->sel + 1 < d->n) { d->sel++; if (!d->ents[d->sel].is_dir) strcpy(d->name, d->ents[d->sel].name); }
    else if (k == UI_KEY_UP && d->sel > 0) { d->sel--; if (!d->ents[d->sel].is_dir) strcpy(d->name, d->ents[d->sel].name); }
    else if (k >= 32 && k < 127 && n < (int)sizeof(d->name) - 1) { d->name[n] = (char)k; d->name[n + 1] = 0; }
    int rows = (DLG_H - 78 - 96) / DLG_ROW;
    if (d->sel >= d->scroll + rows) d->scroll = d->sel - rows + 1;
    if (d->sel >= 0 && d->sel < d->scroll) d->scroll = d->sel;
    return 0;
}
