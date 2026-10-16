#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"

/* Files: browse folders as icons, open things in the right app, make folders, delete. */

#define MAXE 160
#define SIDE_W 168
#define BAR_H 48
#define STAT_H 26
#define TILE_W 104
#define TILE_H 96

typedef struct {
    char dir[160];
    char back[8][160];
    int nback;
    uihost_entry_t e[MAXE];
    int n, sel, scroll;
    uint32_t last_ms;
    char msg[80];
    int confirm_delete;
} files_t;

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int has_ext(const char *name, const char *exts)
{
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    if (!dot) return 0;
    /* exts: "png jpg jpeg" */
    const char *e = exts;
    while (*e) {
        const char *w = e;
        while (*e && *e != ' ') e++;
        int len = (int)(e - w), ok = 1;
        if ((int)strlen(dot + 1) != len) ok = 0;
        for (int i = 0; ok && i < len; i++) if (lower(dot[1 + i]) != w[i]) ok = 0;
        if (ok) return 1;
        while (*e == ' ') e++;
    }
    return 0;
}

#define IMG_EXT  "png jpg jpeg jfif"
#define TEXT_EXT "txt md c h py sh cfg conf log json ini csv tsharp ts rc"

static const char *icon_for(const uihost_entry_t *e)
{
    if (e->is_dir) return "icon48_files";
    if (has_ext(e->name, IMG_EXT)) return "icon48_viewer";
    if (has_ext(e->name, "pdf")) return "icon48_pdf";
    if (has_ext(e->name, "mp3 wav aac m4a")) return "icon48_media";
    if (has_ext(e->name, "t")) return "icon48_terminal";
    return "icon48_notepad";
}

static void load(files_t *f)
{
    f->n = uihost_list(f->dir, f->e, MAXE);
    if (f->n < 0) { f->n = 0; strcpy(f->msg, "This folder cannot be read"); }
    for (int i = 1; i < f->n; i++) {             /* folders first, then by name */
        uihost_entry_t t = f->e[i];
        int j = i - 1;
        while (j >= 0 && (f->e[j].is_dir < t.is_dir || (f->e[j].is_dir == t.is_dir && strcmp(f->e[j].name, t.name) > 0))) {
            f->e[j + 1] = f->e[j]; j--;
        }
        f->e[j + 1] = t;
    }
    f->sel = -1;
    f->scroll = 0;
    f->confirm_delete = 0;
}

static void go(files_t *f, const char *dir, int remember)
{
    if (remember) {
        if (f->nback == 8) { for (int i = 1; i < 8; i++) strcpy(f->back[i - 1], f->back[i]); f->nback--; }
        strcpy(f->back[f->nback++], f->dir);
    }
    strncpy(f->dir, dir, sizeof(f->dir) - 1);
    f->msg[0] = 0;
    load(f);
}

static void up(files_t *f)
{
    char d[160];
    strcpy(d, f->dir);
    int n = (int)strlen(d);
    while (n > 1 && d[n - 1] == '/') d[--n] = 0;
    while (n > 1 && d[n - 1] != '/') d[--n] = 0;
    if (n > 1) d[n - 1] = 0;
    if (!d[0]) strcpy(d, "/");
    go(f, d, 1);
}

static void open_entry(files_t *f, int i)
{
    char p[224];
    ui_path_join(p, f->dir, f->e[i].name);
    if (f->e[i].is_dir) { go(f, p, 1); return; }
    if (has_ext(f->e[i].name, IMG_EXT)) uihost_open("viewer", p);
    else if (has_ext(f->e[i].name, "pdf")) uihost_open("pdf", p);
    else if (has_ext(f->e[i].name, "mp3 wav aac m4a")) uihost_open("music", p);
    else if (has_ext(f->e[i].name, "t")) uihost_open("run", p);
    else uihost_open("notepad", p);
}

static const char *const places[4][2] = { { "Home", "" }, { "Computer", "/" }, { "Programs", "/mnt/programs" }, { "Temporary", "/tmp" } };

static void f_init(uiapp_t *a, const char *arg)
{
    files_t *f = a->st;
    strncpy(f->dir, arg ? arg : uihost_home(), sizeof(f->dir) - 1);
    load(f);
}

static void tile_pos(uiapp_t *a, int i, int *tx, int *ty)
{
    int cols = (a->w - SIDE_W - 20) / TILE_W;
    if (cols < 1) cols = 1;
    *tx = SIDE_W + 10 + (i % cols) * TILE_W;
    *ty = BAR_H + 10 + (i / cols) * TILE_H;
}

static void f_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    files_t *f = a->st;
    strcpy(a->title, f->dir[0] && strcmp(f->dir, "/") ? ui_basename(f->dir) : "Computer");
    /* sidebar */
    gfx_fill(s, x, y, SIDE_W, h, 0x202127);
    gfx_text(s, &ui_font_small, x + 16, y + 14, "FAVOURITES", 0x6B6E79, 255);
    for (int i = 0; i < 4; i++) {
        const char *target = places[i][1][0] ? places[i][1] : uihost_home();
        int on = !strcmp(target, f->dir);
        int py = y + 34 + i * 32;
        if (on) gfx_rrect(s, x + 8, py, SIDE_W - 16, 28, 7, 0x34363F, 255);
        gfx_blit(s, gfx_image(i == 0 ? "icon20_about" : i == 1 ? "icon20_diskmgr" : i == 2 ? "icon20_terminal" : "icon20_files"), x + 16, py + 4, 255);
        gfx_text(s, &ui_font_regular, x + 44, py + 5, places[i][0], UI_TEXT, 255);
    }
    gfx_fill(s, x + SIDE_W, y, 1, h, 0x000000);

    /* toolbar */
    int bx = x + SIDE_W + 1, bw = w - SIDE_W - 1;
    gfx_fill(s, bx, y, bw, BAR_H, 0x26272E);
    ui_button(s, bx + 10, y + 10, 32, 28, "\xE2\x86\x90", UI_CARD2, f->nback ? UI_TEXT : 0x5C5F6A, 0);
    ui_button(s, bx + 46, y + 10, 32, 28, "\xE2\x86\x91", UI_CARD2, UI_TEXT, 0);
    gfx_surface_t t = *s;
    gfx_clip(&t, bx + 88, y, bw - 88 - 230, BAR_H);
    gfx_text(&t, &ui_font_medium, bx + 90, y + 14, f->dir, UI_TEXT, 255);
    int sel_img = f->sel >= 0 && !f->e[f->sel].is_dir && has_ext(f->e[f->sel].name, IMG_EXT);
    ui_button(s, bx + bw - 222, y + 10, 104, 28, sel_img ? "Wallpaper" : "New Folder", UI_CARD2, UI_TEXT, 0);
    ui_button(s, bx + bw - 112, y + 10, 100, 28, f->confirm_delete ? "Really?" : "Delete",
              f->confirm_delete ? 0x7F1D1D : UI_CARD2, f->sel >= 0 ? (f->confirm_delete ? 0xFCA5A5 : UI_TEXT) : 0x5C5F6A, 0);
    gfx_hline(s, bx, y + BAR_H - 1, bw, 0, 120);

    /* the icons */
    gfx_fill(s, bx, y + BAR_H, bw, h - BAR_H - STAT_H, UI_BG);
    gfx_surface_t c = *s;
    gfx_clip(&c, bx, y + BAR_H, bw, h - BAR_H - STAT_H);
    for (int i = 0; i < f->n; i++) {
        int tx, ty;
        tile_pos(a, i, &tx, &ty);
        ty -= f->scroll;
        tx += x; ty += y;
        if (ty > y + h || ty + TILE_H < y + BAR_H) continue;
        if (i == f->sel) gfx_rrect(&c, tx + 4, ty, TILE_W - 8, TILE_H - 6, 10, 0x2E4A7A, 255);
        gfx_blit(&c, gfx_image(icon_for(&f->e[i])), tx + (TILE_W - 48) / 2, ty + 6, 255);
        /* the name, shortened to fit */
        char nm[24];
        strncpy(nm, f->e[i].name, 23); nm[23] = 0;
        while (gfx_text_width(&ui_font_small, nm) > TILE_W - 10 && strlen(nm) > 4) { int L = (int)strlen(nm); nm[L - 4] = '.'; nm[L - 3] = '.'; nm[L - 2] = 0; }
        gfx_text_center(&c, &ui_font_small, tx, ty + 60, TILE_W, nm, UI_TEXT, 255);
    }
    if (!f->n) gfx_text_center(s, &ui_font_regular, bx, y + h / 2, bw, "This folder is empty", UI_DIM, 255);

    /* status */
    int sy = y + h - STAT_H;
    gfx_fill(s, bx, sy, bw, STAT_H, 0x24252C);
    char st[96] = "";
    if (f->msg[0]) strcpy(st, f->msg);
    else if (f->sel >= 0) {
        strcpy(st, f->e[f->sel].name);
        if (!f->e[f->sel].is_dir) { strcat(st, "  \xC2\xB7  "); ui_size(st, f->e[f->sel].size); }
    } else { ui_num(st, f->n); strcat(st, f->n == 1 ? " item" : " items"); }
    gfx_text(s, &ui_font_small, bx + 12, sy + 5, st, UI_DIM, 255);
}

static void f_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    files_t *f = a->st;
    a->dirty = 1;
    if (px < SIDE_W) {
        int i = (py - 34) / 32;
        if (py >= 34 && i >= 0 && i < 4) go(f, places[i][1][0] ? places[i][1] : uihost_home(), 1);
        return;
    }
    int bx = SIDE_W + 1, bw = a->w - SIDE_W - 1;
    if (py < BAR_H) {
        if (ui_hit(px, py, bx + 10, 10, 32, 28) && f->nback) { f->nback--; char d[160]; strcpy(d, f->back[f->nback]); go(f, d, 0); }
        else if (ui_hit(px, py, bx + 46, 10, 32, 28)) up(f);
        else if (ui_hit(px, py, bx + bw - 222, 10, 104, 28)) {
            if (f->sel >= 0 && !f->e[f->sel].is_dir && has_ext(f->e[f->sel].name, IMG_EXT)) {
                char p[224];
                ui_path_join(p, f->dir, f->e[f->sel].name);
                strcpy(f->msg, uihost_set_wallpaper(p) == 0 ? "Wallpaper changed" : "Could not use that picture");
            } else {
                char p[224], name[32] = "New Folder";
                for (int k = 2; k < 50; k++) {
                    ui_path_join(p, f->dir, name);
                    int clash = 0;
                    for (int i = 0; i < f->n; i++) if (!strcmp(f->e[i].name, name)) clash = 1;
                    if (!clash) break;
                    strcpy(name, "New Folder "); ui_num(name, k);
                }
                if (uihost_mkdir(p) == 0) { load(f); strcpy(f->msg, "Folder created"); }
                else strcpy(f->msg, "Could not create a folder here");
            }
        } else if (ui_hit(px, py, bx + bw - 112, 10, 100, 28) && f->sel >= 0) {
            if (!f->confirm_delete) { f->confirm_delete = 1; return; }
            char p[224];
            ui_path_join(p, f->dir, f->e[f->sel].name);
            int r = uihost_delete(p);
            load(f);
            strcpy(f->msg, r == 0 ? "Deleted" : "Could not delete it (a folder must be empty)");
        }
        return;
    }
    f->confirm_delete = 0;
    int hit = -1;
    for (int i = 0; i < f->n; i++) {
        int tx, ty;
        tile_pos(a, i, &tx, &ty);
        ty -= f->scroll;
        if (ui_hit(px, py, tx + 4, ty, TILE_W - 8, TILE_H - 6)) { hit = i; break; }
    }
    uint32_t now = uihost_ms();
    if (hit >= 0 && hit == f->sel && now - f->last_ms < 450) { f->last_ms = 0; open_entry(f, hit); return; }
    f->sel = hit;
    f->last_ms = now;
    f->msg[0] = 0;
}

static void f_key(uiapp_t *a, int k)
{
    files_t *f = a->st;
    int cols = (a->w - SIDE_W - 20) / TILE_W;
    if (cols < 1) cols = 1;
    if (k == UI_KEY_RIGHT && f->sel + 1 < f->n) f->sel++;
    else if (k == UI_KEY_LEFT && f->sel > 0) f->sel--;
    else if (k == UI_KEY_DOWN && f->sel + cols < f->n) f->sel += f->sel < 0 ? 1 : cols;
    else if (k == UI_KEY_UP && f->sel - cols >= 0) f->sel -= cols;
    else if (k == '\n' && f->sel >= 0) open_entry(f, f->sel);
    else if (k == '\b') up(f);
    a->dirty = 1;
}

static void f_wheel(uiapp_t *a, int d)
{
    files_t *f = a->st;
    f->scroll -= d * 40;
    int cols = (a->w - SIDE_W - 20) / TILE_W;
    if (cols < 1) cols = 1;
    int rows = (f->n + cols - 1) / cols;
    int max = rows * TILE_H - (a->h - BAR_H - STAT_H - 20);
    if (f->scroll > max) f->scroll = max;
    if (f->scroll < 0) f->scroll = 0;
    a->dirty = 1;
}

const uiapp_class_t app_files = {
    "files", "Files", "files", 760, 460, 1, sizeof(files_t),
    f_init, f_draw, f_press, 0, 0, f_key, f_wheel, 0, 0,
};
