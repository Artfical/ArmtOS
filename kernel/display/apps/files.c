#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"

/*
 * Files, made to work like the Finder: a sidebar of favourites and locations, back and forward,
 * icon and list views, a search field, a path bar, Return to rename, double-click to open in
 * the right app, and the selected picture as the wallpaper.
 */

#define MAXE 160
#define SIDE_W 180
#define BAR_H 52
#define PATH_H 28
#define TILE_W 100
#define TILE_H 100
#define ROW_H 24

typedef struct {
    char dir[160];
    char hist[10][160];
    int nhist, pos;
    uihost_entry_t all[MAXE];
    int nall;
    int shown[MAXE];
    int n, sel, scroll;
    int list_view;
    char search[32];
    int search_focus;
    int renaming;
    char rename[64];
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
    const char *e = exts;
    while (*e) {
        const char *w = e;
        while (*e && *e != ' ') e++;
        int len = (int)(e - w), ok = (int)strlen(dot + 1) == len;
        for (int i = 0; ok && i < len; i++) if (lower(dot[1 + i]) != w[i]) ok = 0;
        if (ok) return 1;
        while (*e == ' ') e++;
    }
    return 0;
}

#define IMG_EXT  "png jpg jpeg jfif"
#define TEXT_EXT "txt md c h py sh cfg conf log json ini csv ts rc"
#define AUD_EXT  "mp3 wav aac m4a"

static const char *kind_key(const uihost_entry_t *e)
{
    if (e->is_dir) return "folder";
    if (has_ext(e->name, IMG_EXT)) return "image";
    if (has_ext(e->name, "pdf")) return "pdf";
    if (has_ext(e->name, AUD_EXT)) return "audio";
    if (has_ext(e->name, "t")) return "app";
    if (has_ext(e->name, TEXT_EXT) || !strchr(e->name, '.')) return "text";
    return "generic";
}

static const char *kind_name(const uihost_entry_t *e)
{
    const char *k = kind_key(e);
    if (!strcmp(k, "folder")) return "Folder";
    if (!strcmp(k, "image")) return "Image";
    if (!strcmp(k, "pdf")) return "PDF document";
    if (!strcmp(k, "audio")) return "Audio";
    if (!strcmp(k, "app")) return "tOS program";
    if (!strcmp(k, "text")) return "Plain text";
    return "Document";
}

static const gfx_image_t *doc_icon(const uihost_entry_t *e, int big)
{
    char n[32];
    strcpy(n, big ? "doc64_" : "doc20_");
    strcat(n, kind_key(e));
    return gfx_image(n);
}

static int matches(const char *name, const char *q)
{
    if (!q[0]) return 1;
    int ql = (int)strlen(q);
    for (const char *p = name; *p; p++) {
        int i = 0;
        while (i < ql && p[i] && lower(p[i]) == lower(q[i])) i++;
        if (i == ql) return 1;
    }
    return 0;
}

static void filter(files_t *f)
{
    f->n = 0;
    /* dot files stay hidden, as in the Finder */
    for (int i = 0; i < f->nall; i++)
        if (f->all[i].name[0] != '.' && matches(f->all[i].name, f->search)) f->shown[f->n++] = i;
    if (f->sel >= f->n) f->sel = -1;
}

static void load(files_t *f)
{
    f->nall = uihost_list(f->dir, f->all, MAXE);
    if (f->nall < 0) { f->nall = 0; strcpy(f->msg, "This folder cannot be read"); }
    for (int i = 1; i < f->nall; i++) {
        uihost_entry_t t = f->all[i];
        int j = i - 1;
        while (j >= 0 && (f->all[j].is_dir < t.is_dir || (f->all[j].is_dir == t.is_dir && strcmp(f->all[j].name, t.name) > 0))) {
            f->all[j + 1] = f->all[j]; j--;
        }
        f->all[j + 1] = t;
    }
    f->sel = -1; f->scroll = 0; f->confirm_delete = 0; f->renaming = 0;
    filter(f);
}

static void go(files_t *f, const char *dir)
{
    if (f->pos < f->nhist - 1) f->nhist = f->pos + 1;
    if (f->nhist == 10) { for (int i = 1; i < 10; i++) strcpy(f->hist[i - 1], f->hist[i]); f->nhist--; }
    strncpy(f->hist[f->nhist], dir, 159);
    f->pos = f->nhist++;
    strncpy(f->dir, dir, sizeof(f->dir) - 1);
    f->search[0] = 0; f->msg[0] = 0;
    load(f);
}

static void step_history(files_t *f, int d)
{
    int np = f->pos + d;
    if (np < 0 || np >= f->nhist) return;
    f->pos = np;
    strcpy(f->dir, f->hist[np]);
    f->search[0] = 0;
    load(f);
}

static void parent(const char *dir, char *out)
{
    strcpy(out, dir);
    int n = (int)strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = 0;
    while (n > 1 && out[n - 1] != '/') out[--n] = 0;
    if (n > 1) out[n - 1] = 0;
    if (!out[0]) strcpy(out, "/");
}

static uihost_entry_t *sel_entry(files_t *f) { return f->sel >= 0 && f->sel < f->n ? &f->all[f->shown[f->sel]] : 0; }

static void open_entry(files_t *f, uihost_entry_t *e)
{
    char p[224];
    ui_path_join(p, f->dir, e->name);
    if (e->is_dir) { go(f, p); return; }
    const char *k = kind_key(e);
    if (!strcmp(k, "image")) uihost_open("viewer", p);
    else if (!strcmp(k, "pdf")) uihost_open("pdf", p);
    else if (!strcmp(k, "audio")) uihost_open("music", p);
    else if (!strcmp(k, "app")) uihost_open("run", p);
    else uihost_open("notepad", p);
}

typedef struct { const char *label, *glyph, *sub; int abs; } place_t;
static const place_t places[] = {
    { "Home", "glyph_house", "", 0 }, { "Documents", "glyph_file-text", "Documents", 0 },
    { "Downloads", "glyph_download", "Downloads", 0 }, { "Pictures", "glyph_image", "Pictures", 0 },
    { "tOS", "glyph_hard-drive", "/", 1 }, { "Programs", "glyph_folder", "/mnt/programs", 1 }, { "Temporary", "glyph_folder", "/tmp", 1 },
};
#define NPLACES 7
#define FIRST_LOCATION 4

static void place_path(int i, char *out)
{
    if (places[i].abs) strcpy(out, places[i].sub);
    else if (places[i].sub[0]) ui_path_join(out, uihost_home(), places[i].sub);
    else strcpy(out, uihost_home());
}

static int place_y(int i) { return i < FIRST_LOCATION ? 34 + i * 28 : 34 + i * 28 + 34; }

/* removable disks follow the locations, each with an eject button, like the Finder's sidebar */
static uihost_volume_t vols[4];
static int nvols = -1;
static uint32_t vols_ms;

static void refresh_vols(void) { nvols = uihost_volumes(vols, 4); vols_ms = uihost_ms(); }

static int under(const char *dir, const char *root)
{
    int l = (int)strlen(root);
    return !strncmp(dir, root, (size_t)l) && (dir[l] == 0 || dir[l] == '/');
}

static void eject(files_t *f, int i)
{
    char path[64], name[32];
    strcpy(path, vols[i].path);
    strcpy(name, vols[i].name);
    if (under(f->dir, path)) go(f, uihost_home());       /* step out of it first */
    if (uihost_eject(path) != 0) {
        char body[160] = "The disk \xE2\x80\x9C";
        strcat(body, name);
        strcat(body, "\xE2\x80\x9D wasn\xE2\x80\x99t ejected because one or more programs may be using it.");
        uihost_notify("files", "doc48_disk", "Disk in use", body, 0, 0);
    }
    refresh_vols();
}

static void f_init(uiapp_t *a, const char *arg)
{
    files_t *f = a->st;
    go(f, arg ? arg : uihost_home());
}

static int cols_of(uiapp_t *a) { int c = (a->w - SIDE_W - 20) / TILE_W; return c < 1 ? 1 : c; }

static void tile_pos(uiapp_t *a, int i, int *tx, int *ty)
{
    int cols = cols_of(a);
    *tx = SIDE_W + 10 + (i % cols) * TILE_W;
    *ty = BAR_H + 12 + (i / cols) * TILE_H;
}

static void f_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    files_t *f = a->st;
    strcpy(a->title, strcmp(f->dir, "/") ? ui_basename(f->dir) : "tOS");

    gfx_fill(s, x, y, SIDE_W, h, UI_SIDEBAR);
    gfx_text(s, &ui_font_small, x + 16, y + 14, "Favourites", UI_DIM, 255);
    gfx_text(s, &ui_font_small, x + 16, y + place_y(FIRST_LOCATION) - 20, "Locations", UI_DIM, 255);
    for (int i = 0; i < NPLACES; i++) {
        char p[160];
        place_path(i, p);
        int on = !strcmp(p, f->dir), py = y + place_y(i);
        if (on) gfx_rrect(s, x + 8, py, SIDE_W - 16, 26, 6, ui_theme.dark ? 0x3A3C46 : 0xD8D8DE, 255);
        const gfx_image_t *g = gfx_image(places[i].glyph);
        if (g) gfx_blit_tinted(s, g, x + 18, py + 5, UI_ACCENT, 255);
        gfx_text(s, &ui_font_regular, x + 44, py + 4, places[i].label, UI_TEXT, 255);
    }
    if (nvols < 0) refresh_vols();
    for (int i = 0; i < nvols; i++) {
        int on = !strcmp(vols[i].path, f->dir), py = y + place_y(NPLACES + i);
        if (on) gfx_rrect(s, x + 8, py, SIDE_W - 16, 26, 6, ui_theme.dark ? 0x3A3C46 : 0xD8D8DE, 255);
        gfx_blit_tinted(s, gfx_image("glyph_hard-drive"), x + 18, py + 5, UI_DIM, 255);
        gfx_surface_t lc = *s;
        gfx_clip(&lc, x + 44, py, SIDE_W - 44 - 34, 26);
        gfx_text(&lc, &ui_font_regular, x + 44, py + 4, vols[i].name, UI_TEXT, 255);
        gfx_blit_tinted(s, gfx_image("glyph_eject"), x + SIDE_W - 32, py + 5, UI_DIM, 255);
    }
    gfx_fill(s, x + SIDE_W, y, 1, h, UI_LINE);

    int bx = x + SIDE_W + 1, bw = w - SIDE_W - 1;
    gfx_fill(s, bx, y, bw, BAR_H, UI_TOOLBAR);
    gfx_blit_tinted(s, gfx_image("glyph_chevron-left"), bx + 14, y + 18, f->pos > 0 ? UI_TEXT : UI_DIM, f->pos > 0 ? 255 : 110);
    gfx_blit_tinted(s, gfx_image("glyph_chevron-right"), bx + 40, y + 18, f->pos < f->nhist - 1 ? UI_TEXT : UI_DIM, f->pos < f->nhist - 1 ? 255 : 110);
    gfx_surface_t t = *s;
    gfx_clip(&t, bx + 70, y, bw - 70 - 330, BAR_H);
    gfx_text(&t, &ui_font_medium, bx + 72, y + 16, a->title, UI_TEXT, 255);
    int rx = bx + bw - 12 - 170;
    gfx_rrect(s, rx, y + 12, 170, 28, 7, UI_FIELD, 255);
    if (f->search_focus) gfx_rrect_outline(s, rx - 1, y + 11, 172, 30, 8, UI_ACCENT, 255);
    else gfx_rrect_outline(s, rx, y + 12, 170, 28, 7, UI_LINE, 255);
    gfx_blit_tinted(s, gfx_image("glyph_search"), rx + 8, y + 18, UI_DIM, 255);
    gfx_text(s, &ui_font_regular, rx + 30, y + 17, f->search[0] ? f->search : "Search", f->search[0] ? UI_TEXT : UI_DIM, 255);
    ui_button(s, rx - 40, y + 12, 32, 28, "", f->confirm_delete ? 0x7F1D1D : UI_CARD2, UI_TEXT, 0);
    gfx_blit_tinted(s, gfx_image("glyph_x"), rx - 32, y + 18, f->confirm_delete ? 0xFCA5A5 : (sel_entry(f) ? UI_TEXT : UI_DIM), 255);
    ui_button(s, rx - 80, y + 12, 32, 28, "", UI_CARD2, UI_TEXT, 0);
    gfx_blit_tinted(s, gfx_image("glyph_folder"), rx - 72, y + 18, UI_TEXT, 255);
    int vx = rx - 156;
    gfx_rrect(s, vx, y + 12, 68, 28, 7, UI_CARD2, 255);
    gfx_rrect(s, vx + 2 + f->list_view * 32, y + 14, 32, 24, 6, ui_theme.dark ? 0x5A5C66 : 0xFFFFFF, 255);
    gfx_blit_tinted(s, gfx_image("glyph_layout-grid"), vx + 10, y + 18, UI_TEXT, 255);
    gfx_blit_tinted(s, gfx_image("glyph_list"), vx + 42, y + 18, UI_TEXT, 255);
    gfx_hline(s, bx, y + BAR_H - 1, bw, 0x000000, ui_theme.dark ? 120 : 30);

    int cy = y + BAR_H, ch = h - BAR_H - PATH_H;
    gfx_fill(s, bx, cy, bw, ch, UI_BG);
    gfx_surface_t c = *s;
    gfx_clip(&c, bx, cy, bw, ch);
    if (!f->list_view) {
        for (int i = 0; i < f->n; i++) {
            uihost_entry_t *e = &f->all[f->shown[i]];
            int tx, ty;
            tile_pos(a, i, &tx, &ty);
            ty -= f->scroll;
            tx += x; ty += y;
            if (ty > y + h || ty + TILE_H < cy) continue;
            int on = i == f->sel;
            if (on) gfx_rrect(&c, tx + (TILE_W - 74) / 2, ty - 2, 74, 70, 8, ui_theme.dark ? 0x3A3C46 : 0xDCDCE2, 255);
            gfx_blit(&c, doc_icon(e, 1), tx + (TILE_W - 64) / 2, ty + 1, 255);
            if (on && f->renaming) {
                gfx_rrect(&c, tx + 2, ty + 70, TILE_W - 4, 22, 4, UI_FIELD, 255);
                gfx_rrect_outline(&c, tx + 2, ty + 70, TILE_W - 4, 22, 4, UI_ACCENT, 255);
                int tw2 = gfx_text_width(&ui_font_small, f->rename);
                gfx_text(&c, &ui_font_small, tx + (TILE_W - tw2) / 2, ty + 73, f->rename, UI_TEXT, 255);
                continue;
            }
            char nm[28];
            strncpy(nm, e->name, 27); nm[27] = 0;
            while (gfx_text_width(&ui_font_small, nm) > TILE_W - 8 && strlen(nm) > 5) { int L = (int)strlen(nm); nm[L - 4] = '.'; nm[L - 3] = '.'; nm[L - 2] = 0; }
            int tw2 = gfx_text_width(&ui_font_small, nm);
            if (on) gfx_rrect(&c, tx + (TILE_W - tw2) / 2 - 6, ty + 70, tw2 + 12, 20, 5, UI_ACCENT, 255);
            gfx_text(&c, &ui_font_small, tx + (TILE_W - tw2) / 2, ty + 72, nm, on ? 0xFFFFFF : UI_TEXT, 255);
        }
    } else {
        int hy = cy;
        gfx_text(&c, &ui_font_small, bx + 40, hy + 6, "Name", UI_DIM, 255);
        gfx_text(&c, &ui_font_small, bx + bw - 250, hy + 6, "Size", UI_DIM, 255);
        gfx_text(&c, &ui_font_small, bx + bw - 150, hy + 6, "Kind", UI_DIM, 255);
        gfx_hline(&c, bx + 10, hy + 25, bw - 20, UI_LINE, 255);
        for (int i = 0; i < f->n; i++) {
            uihost_entry_t *e = &f->all[f->shown[i]];
            int ry = hy + 28 + i * ROW_H - f->scroll;
            if (ry + ROW_H < hy + 26 || ry > cy + ch) continue;
            int on = i == f->sel;
            if (on) gfx_rrect(&c, bx + 8, ry, bw - 16, ROW_H, 5, UI_ACCENT, 255);
            else if (i % 2) gfx_fill(&c, bx + 8, ry, bw - 16, ROW_H, ui_theme.dark ? 0x232429 : 0xF4F5F7);
            uint32_t tc = on ? 0xFFFFFF : UI_TEXT, dc = on ? 0xDCE8FF : UI_DIM;
            gfx_blit(&c, doc_icon(e, 0), bx + 14, ry + 2, 255);
            if (on && f->renaming) {
                gfx_rrect(&c, bx + 38, ry + 2, 240, 20, 4, UI_FIELD, 255);
                gfx_text(&c, &ui_font_regular, bx + 44, ry + 4, f->rename, UI_TEXT, 255);
            } else gfx_text(&c, &ui_font_regular, bx + 40, ry + 4, e->name, tc, 255);
            char sz[16] = "";
            if (e->is_dir) strcpy(sz, "--"); else ui_size(sz, e->size);
            gfx_text(&c, &ui_font_regular, bx + bw - 250, ry + 4, sz, dc, 255);
            gfx_text(&c, &ui_font_regular, bx + bw - 150, ry + 4, kind_name(e), dc, 255);
        }
    }
    if (!f->n) gfx_text_center(s, &ui_font_regular, bx, cy + ch / 2 - 10, bw, f->search[0] ? "No results" : "This folder is empty", UI_DIM, 255);

    int py = y + h - PATH_H;
    gfx_fill(s, bx, py, bw, PATH_H, UI_STATUS);
    gfx_hline(s, bx, py, bw, UI_LINE, 255);
    int pxx = bx + 12;
    char comp[160];
    strcpy(comp, f->dir);
    gfx_blit(s, gfx_image("doc20_folder"), pxx, py + 4, 255);
    pxx += 24;
    pxx += gfx_text(s, &ui_font_small, pxx, py + 7, "tOS", UI_DIM, 255);
    for (char *p = comp; *p; ) {
        while (*p == '/') p++;
        if (!*p) break;
        char *q = p;
        while (*q && *q != '/') q++;
        char save = *q;
        *q = 0;
        pxx += gfx_text(s, &ui_font_small, pxx + 4, py + 7, "\xE2\x80\xBA", UI_DIM, 255) + 10;
        pxx += gfx_text(s, &ui_font_small, pxx, py + 7, p, UI_DIM, 255);
        *q = save;
        p = q;
    }
    char st[96] = "";
    if (f->msg[0]) strcpy(st, f->msg);
    else { ui_num(st, f->n); strcat(st, f->n == 1 ? " item" : " items"); }
    gfx_text(s, &ui_font_small, bx + bw - 12 - gfx_text_width(&ui_font_small, st), py + 7, st, UI_DIM, 255);
}

static void do_new_folder(files_t *f)
{
    char p[224], name[32] = "untitled folder";
    for (int k = 2; k < 50; k++) {
        int clash = 0;
        for (int i = 0; i < f->nall; i++) if (!strcmp(f->all[i].name, name)) clash = 1;
        if (!clash) break;
        strcpy(name, "untitled folder "); ui_num(name, k);
    }
    ui_path_join(p, f->dir, name);
    if (uihost_mkdir(p) != 0) { strcpy(f->msg, "Could not create a folder here"); return; }
    load(f);
    for (int i = 0; i < f->n; i++) if (!strcmp(f->all[f->shown[i]].name, name)) { f->sel = i; f->renaming = 1; strcpy(f->rename, name); }
}

static void do_delete(files_t *f)
{
    uihost_entry_t *e = sel_entry(f);
    if (!e) return;
    if (!f->confirm_delete) { f->confirm_delete = 1; strcpy(f->msg, "Click again to delete"); return; }
    char p[224];
    ui_path_join(p, f->dir, e->name);
    int r = uihost_delete(p);
    load(f);
    strcpy(f->msg, r == 0 ? "Deleted" : "Could not delete it (a folder must be empty)");
}

static void finish_rename(files_t *f)
{
    uihost_entry_t *e = sel_entry(f);
    f->renaming = 0;
    if (!e || !f->rename[0] || !strcmp(e->name, f->rename)) return;
    char a[224], b[224];
    ui_path_join(a, f->dir, e->name);
    ui_path_join(b, f->dir, f->rename);
    if (uihost_rename(a, b) != 0) { strcpy(f->msg, "Could not rename it"); return; }
    char keep[64];
    strcpy(keep, f->rename);
    load(f);
    for (int i = 0; i < f->n; i++) if (!strcmp(f->all[f->shown[i]].name, keep)) f->sel = i;
}

static void f_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    files_t *f = a->st;
    a->dirty = 1;
    if (f->renaming) finish_rename(f);
    if (px < SIDE_W) {
        for (int i = 0; i < NPLACES; i++)
            if (py >= place_y(i) && py < place_y(i) + 26) { char p[160]; place_path(i, p); go(f, p); }
        for (int i = 0; i < nvols; i++)
            if (py >= place_y(NPLACES + i) && py < place_y(NPLACES + i) + 26) {
                if (px >= SIDE_W - 40) eject(f, i);
                else go(f, vols[i].path);
            }
        return;
    }
    int bx = SIDE_W + 1, bw = a->w - SIDE_W - 1;
    if (py < BAR_H) {
        int rx = bx + bw - 12 - 170;
        f->search_focus = ui_hit(px, py, rx, 12, 170, 28);
        if (ui_hit(px, py, bx + 8, 10, 28, 32)) step_history(f, -1);
        else if (ui_hit(px, py, bx + 36, 10, 28, 32)) step_history(f, 1);
        else if (ui_hit(px, py, rx - 40, 12, 32, 28)) do_delete(f);
        else if (ui_hit(px, py, rx - 80, 12, 32, 28)) do_new_folder(f);
        else if (ui_hit(px, py, rx - 156, 12, 68, 28)) { f->list_view = px >= rx - 156 + 34; f->scroll = 0; }
        return;
    }
    f->search_focus = 0;
    if (py >= a->h - PATH_H) return;
    int hit = -1;
    if (!f->list_view) {
        for (int i = 0; i < f->n; i++) {
            int tx, ty;
            tile_pos(a, i, &tx, &ty);
            ty -= f->scroll;
            if (ui_hit(px, py, tx + 6, ty - 2, TILE_W - 12, TILE_H - 6)) { hit = i; break; }
        }
    } else {
        int i = (py - BAR_H - 28 + f->scroll) / ROW_H;
        if (py >= BAR_H + 28 && i >= 0 && i < f->n) hit = i;
    }
    uint32_t now = uihost_ms();
    if (hit >= 0 && hit == f->sel && now - f->last_ms < 450) { f->last_ms = 0; open_entry(f, sel_entry(f)); return; }
    if (hit != f->sel) f->confirm_delete = 0;
    f->sel = hit;
    f->last_ms = now;
    if (!f->confirm_delete) f->msg[0] = 0;
    uihost_entry_t *e = sel_entry(f);
    if (e && !e->is_dir) { strcpy(f->msg, e->name); strcat(f->msg, "  \xC2\xB7  "); ui_size(f->msg, e->size); }
}

static void f_key(uiapp_t *a, int k)
{
    files_t *f = a->st;
    a->dirty = 1;
    if (f->renaming) {
        int n = (int)strlen(f->rename);
        if (k == '\n') finish_rename(f);
        else if (k == UI_KEY_ESC) f->renaming = 0;
        else if (k == '\b') { if (n) f->rename[n - 1] = 0; }
        else if (k >= 32 && k < 127 && k != '/' && n < 63) { f->rename[n] = (char)k; f->rename[n + 1] = 0; }
        return;
    }
    if (f->search_focus) {
        int n = (int)strlen(f->search);
        if (k == '\b') { if (n) f->search[n - 1] = 0; }
        else if (k == UI_KEY_ESC) { f->search[0] = 0; f->search_focus = 0; }
        else if (k == '\n') f->search_focus = 0;
        else if (k >= 32 && k < 127 && n < 31) { f->search[n] = (char)k; f->search[n + 1] = 0; }
        filter(f);
        return;
    }
    int cols = f->list_view ? 1 : cols_of(a);
    if (k == UI_KEY_RIGHT && !f->list_view && f->sel + 1 < f->n) f->sel++;
    else if (k == UI_KEY_LEFT && !f->list_view && f->sel > 0) f->sel--;
    else if (k == UI_KEY_DOWN) f->sel = f->sel < 0 ? 0 : (f->sel + cols < f->n ? f->sel + cols : f->sel);
    else if (k == UI_KEY_UP && f->sel - cols >= 0) f->sel -= cols;
    else if (k == '\n' && sel_entry(f)) { f->renaming = 1; strcpy(f->rename, sel_entry(f)->name); }
    else if (k == 'o' && sel_entry(f)) open_entry(f, sel_entry(f));
    else if (k == '\b') { char p[160]; parent(f->dir, p); go(f, p); }
    else if (k == UI_KEY_DEL) do_delete(f);
}

static void f_wheel(uiapp_t *a, int d)
{
    files_t *f = a->st;
    f->scroll -= d * 40;
    int total = f->list_view ? f->n * ROW_H + 28 : ((f->n + cols_of(a) - 1) / cols_of(a)) * TILE_H + 12;
    int max = total - (a->h - BAR_H - PATH_H) + 10;
    if (f->scroll > max) f->scroll = max;
    if (f->scroll < 0) f->scroll = 0;
    a->dirty = 1;
}

/* disks come and go: look twice a second, and leave a disk that was pulled out */
static int f_tick(uiapp_t *a)
{
    files_t *f = a->st;
    if (uihost_ms() - vols_ms < 500) return 0;
    int before = nvols;
    char old[4][64];
    for (int i = 0; i < nvols && i < 4; i++) strcpy(old[i], vols[i].path);
    refresh_vols();
    int changed = before != nvols;
    for (int i = 0; i < nvols && !changed; i++) if (strcmp(old[i], vols[i].path)) changed = 1;
    if (!changed) return 0;
    if (under(f->dir, "/Volumes")) {
        int still = 0;
        for (int i = 0; i < nvols; i++) if (under(f->dir, vols[i].path)) still = 1;
        if (!still) go(f, uihost_home());
    }
    if (!strcmp(f->dir, "/Volumes") || !strcmp(f->dir, "/")) load(f);
    return 1;
}

const uiapp_class_t app_files = {
    "files", "Files", "files", 820, 480, 1, sizeof(files_t),
    f_init, f_draw, f_press, 0, 0, f_key, f_wheel, f_tick, 0,
};
