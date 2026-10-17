#include "wm_notify.h"
#include "uiapp.h"
#include "string.h"
#include "cmos.h"
#include "sound.h"
#include "debugmon.h"

/*
 * Notifications, the way macOS shows them: a banner slides in at the top right with the app's
 * icon, its name and the time, a bold title, two lines of text and up to two buttons. Plain
 * banners leave by themselves after a few seconds; ones with buttons wait for an answer.
 * Everything that arrived stays in Notification Centre until it is cleared. Do Not Disturb
 * keeps the banners and the chime away but the Centre still collects.
 */

#define NMAX 32
#define BANNER_W 356
#define BANNER_MS 6000
#define SLIDE_MS 260
#define MAX_BANNERS 3
#define CENTER_W 360

typedef struct {
    int used, id, gone, result;
    char app[24], icon[32], title[64], body[192], act[2][20];
    int hour, min;
    uint32_t at;             /* uptime when posted */
    int banner;              /* still showing as a banner */
    /* where it was drawn last, for clicks */
    int bx, by, bw, bh, on_screen;
} notif_t;

static notif_t ns[NMAX];
static int next_id = 1;
static int dnd;
static int center_open;
static uint32_t center_at;
static int c_x, c_y, c_w, c_h;          /* the Centre's panel, when open */
static int dnd_x, dnd_y, clear_x, clear_y;
static uint32_t last_anim;

static void copy(char *d, const char *s, int n)
{
    int i = 0;
    if (s) while (s[i] && i < n - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

int notify_post(const char *app, const char *icon, const char *title, const char *body,
                const char *act1, const char *act2)
{
    notif_t *n = 0;
    for (int i = 0; i < NMAX && !n; i++) if (!ns[i].used) n = &ns[i];
    if (!n) {   /* full: the oldest one already answered or closed makes room */
        for (int i = 0; i < NMAX; i++)
            if (ns[i].gone && (!n || ns[i].at < n->at)) n = &ns[i];
        if (!n) for (int i = 0; i < NMAX; i++) if (!ns[i].banner && (!n || ns[i].at < n->at)) n = &ns[i];
        if (!n) return -1;
    }
    memset(n, 0, sizeof(*n));
    n->used = 1;
    n->id = next_id++;
    copy(n->app, app ? app : "System", sizeof(n->app));
    copy(n->icon, icon, sizeof(n->icon));
    copy(n->title, title, sizeof(n->title));
    copy(n->body, body, sizeof(n->body));
    copy(n->act[0], act1, sizeof(n->act[0]));
    copy(n->act[1], act2, sizeof(n->act[1]));
    cmos_time_t t;
    cmos_get_time(&t);
    n->hour = t.hour; n->min = t.minute;
    n->at = debugmon_uptime_ms();
    n->banner = !dnd && !center_open;
    if (n->banner) sound_notify();
    return n->id;
}

static notif_t *by_id(int id)
{
    for (int i = 0; i < NMAX; i++) if (ns[i].used && ns[i].id == id) return &ns[i];
    return 0;
}

int notify_result(int id)
{
    notif_t *n = by_id(id);
    if (!n) return -2;
    return n->result;
}

int notify_dnd(void) { return dnd; }
void notify_set_dnd(int on)
{
    dnd = on ? 1 : 0;
    if (dnd) for (int i = 0; i < NMAX; i++) ns[i].banner = 0;
}
int notify_center_is_open(void) { return center_open; }
void notify_center_toggle(void)
{
    center_open = !center_open;
    center_at = debugmon_uptime_ms();
    if (center_open) for (int i = 0; i < NMAX; i++) ns[i].banner = 0;   /* the Centre shows them now */
}

static void close_notif(notif_t *n, int result)
{
    if (!n->result) n->result = result;
    n->gone = 1;
    n->banner = 0;
}

/* ------------------------------------------------------------------ drawing */

static const gfx_image_t *icon_of(const notif_t *n)
{
    char k[48];
    const gfx_image_t *im = 0;
    if (n->icon[0]) {
        im = gfx_image(n->icon);
        if (!im) { strcpy(k, "icon48_"); strcat(k, n->icon); im = gfx_image(k); }
    }
    if (!im) { strcpy(k, "icon48_"); strcat(k, n->app); im = gfx_image(k); }
    if (!im) im = gfx_image("icon48_settings");
    return im;
}

static void app_label(const notif_t *n, char *out)
{
    /* an app id is shown by its window title ("files" -> "Files") */
    const uiapp_class_t *c = uiapp_find(n->app);
    copy(out, c ? c->title : n->app, 32);
    for (char *p = out; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);   /* macOS writes the app's name in capitals */
}

static void when(const notif_t *n, char *out)
{
    uint32_t ago = (debugmon_uptime_ms() - n->at) / 60000;
    if (ago < 1) { strcpy(out, "now"); return; }
    if (ago < 60) {
        int o = 0;
        if (ago >= 10) out[o++] = (char)('0' + ago / 10);
        out[o++] = (char)('0' + ago % 10);
        strcpy(out + o, "m ago");
        return;
    }
    out[0] = (char)('0' + n->hour / 10); out[1] = (char)('0' + n->hour % 10); out[2] = ':';
    out[3] = (char)('0' + n->min / 10); out[4] = (char)('0' + n->min % 10); out[5] = 0;
}

/* breaks text into at most two lines that fit `w`, the second ending in an ellipsis if cut */
static int wrap(const ui_font_t *f, const char *s, int w, char lines[2][96])
{
    int nl = 0;
    lines[0][0] = lines[1][0] = 0;
    while (*s && nl < 2) {
        while (*s == ' ') s++;
        int best = 0, i = 0;
        char tmp[96];
        while (s[i] && i < 94) {
            tmp[i] = s[i]; tmp[i + 1] = 0;
            if (gfx_text_width(f, tmp) > w) break;
            if (s[i + 1] == ' ' || s[i + 1] == 0) best = i + 1;
            i++;
        }
        if (!s[i] && gfx_text_width(f, tmp) <= w) best = i;
        if (!best) best = i ? i : 1;                 /* one long word: cut it */
        memcpy(lines[nl], s, (size_t)best);
        lines[nl][best] = 0;
        s += best;
        nl++;
    }
    while (*s == ' ') s++;
    if (*s && nl == 2) {
        int l = (int)strlen(lines[1]);
        while (l > 0 && gfx_text_width(f, lines[1]) + gfx_text_width(f, "\xE2\x80\xA6") > w) lines[1][--l] = 0;
        strcat(lines[1], "\xE2\x80\xA6");
    }
    return nl;
}

static int card_height(const notif_t *n, int w)
{
    char l[2][96];
    int nl = n->body[0] ? wrap(&ui_font_regular, n->body, w - 72, l) : 0;
    int h = 54 + nl * 18 + 6;
    if (n->act[0][0] && !n->result) h += 40;
    return h;
}

static void draw_card(gfx_surface_t *s, notif_t *n, int x, int y, int w, int alpha_bg)
{
    int h = card_height(n, w);
    n->bx = x; n->by = y; n->bw = w; n->bh = h; n->on_screen = 1;
    gfx_shadow(s, x, y, w, h, 150);
    gfx_rrect(s, x, y, w, h, 16, ui_theme.dark ? 0x2C2D33 : 0xF6F6F8, alpha_bg);
    gfx_rrect_outline(s, x, y, w, h, 16, ui_theme.dark ? 0xFFFFFF : 0x000000, ui_theme.dark ? 36 : 22);
    const gfx_image_t *im = icon_of(n);
    if (im) gfx_blit_scaled(s, im, x + 14, y + 14, 38, 38, 255);
    char lab[32], t[16];
    app_label(n, lab);
    when(n, t);
    gfx_text(s, &ui_font_small, x + 64, y + 11, lab, ui_theme.dim, 255);
    int tw = gfx_text_width(&ui_font_small, t);
    gfx_text(s, &ui_font_small, x + w - 36 - tw, y + 11, t, ui_theme.dim, 255);
    /* the close button */
    gfx_circle(s, x + w - 18, y + 18, 9, ui_theme.dark ? 0x45464E : 0xE2E2E6, 255);
    const gfx_image_t *xg = gfx_image("glyph_x");
    if (xg) gfx_blit_tinted(s, xg, x + w - 26, y + 10, ui_theme.dim, 255);
    gfx_surface_t c = *s;
    gfx_clip(&c, x + 64, y, w - 64 - 12, h);
    gfx_text(&c, &ui_font_medium, x + 64, y + 30, n->title, ui_theme.text, 255);
    char l[2][96];
    int nl = n->body[0] ? wrap(&ui_font_regular, n->body, w - 72, l) : 0;
    for (int i = 0; i < nl; i++) gfx_text(&c, &ui_font_regular, x + 64, y + 50 + i * 18, l[i], ui_theme.text, 220);
    if (n->act[0][0] && !n->result) {
        int na = n->act[1][0] ? 2 : 1, by = y + h - 40, bw = (w - 64 - 14 - (na - 1) * 8) / na;
        for (int i = 0; i < na; i++) {
            int bx = x + 64 + i * (bw + 8);
            gfx_rrect(s, bx, by, bw, 28, 8, i == 0 ? UI_ACCENT : (ui_theme.dark ? 0x45464E : 0xE4E4E8), 255);
            gfx_text_center(s, &ui_font_medium, bx, by + 6, bw, n->act[i], i == 0 ? 0xFFFFFF : ui_theme.text, 255);
        }
    }
}

static void switch_draw(gfx_surface_t *s, int x, int y, int on)
{
    gfx_rrect(s, x, y, 38, 22, 11, on ? 0x5E5CE6 : (ui_theme.dark ? 0x55565E : 0xD4D4D8), 255);
    gfx_circle(s, on ? x + 27 : x + 11, y + 11, 9, 0xFFFFFF, 255);
}

static const char *const wdays[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const months[12] = { "January", "February", "March", "April", "May", "June", "July",
                                        "August", "September", "October", "November", "December" };

static void draw_center(gfx_surface_t *s, int sw, int sh, int topbar_h)
{
    uint32_t dt = debugmon_uptime_ms() - center_at;
    int slide = dt < SLIDE_MS ? (int)((CENTER_W + 20) * (SLIDE_MS - dt) / SLIDE_MS) : 0;
    c_w = CENTER_W; c_x = sw - CENTER_W - 10 + slide; c_y = topbar_h + 8; c_h = sh - topbar_h - 16;
    gfx_shadow(s, c_x, c_y, c_w, c_h, 160);
    gfx_rrect(s, c_x, c_y, c_w, c_h, 18, ui_theme.dark ? 0x1E1F24 : 0xECECF0, 236);
    gfx_rrect_outline(s, c_x, c_y, c_w, c_h, 18, ui_theme.dark ? 0xFFFFFF : 0x000000, 26);

    cmos_time_t t;
    cmos_get_time(&t);
    static const int k[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int yy = t.year - (t.month < 3);
    int wd = (yy + yy / 4 - yy / 100 + yy / 400 + k[(t.month - 1) % 12] + t.day) % 7;
    char date[40] = "";
    strcat(date, months[(t.month + 11) % 12]);
    strcat(date, " ");
    char d2[4] = { 0 };
    if (t.day >= 10) { d2[0] = (char)('0' + t.day / 10); d2[1] = (char)('0' + t.day % 10); } else d2[0] = (char)('0' + t.day);
    strcat(date, d2);
    gfx_text(s, &ui_font_title, c_x + 20, c_y + 16, wdays[wd], ui_theme.text, 255);
    gfx_text(s, &ui_font_regular, c_x + 20, c_y + 46, date, ui_theme.dim, 255);

    /* Do Not Disturb */
    int y = c_y + 76;
    gfx_rrect(s, c_x + 12, y, c_w - 24, 58, 14, ui_theme.dark ? 0x2C2D33 : 0xFFFFFF, 255);
    gfx_circle(s, c_x + 40, y + 29, 16, dnd ? 0x5E5CE6 : (ui_theme.dark ? 0x45464E : 0xE4E4E8), 255);
    const gfx_image_t *moon = gfx_image("glyph_moon");
    if (moon) gfx_blit_tinted(s, moon, c_x + 32, y + 21, dnd ? 0xFFFFFF : ui_theme.text, 255);
    gfx_text(s, &ui_font_medium, c_x + 66, y + 12, "Do Not Disturb", ui_theme.text, 255);
    gfx_text(s, &ui_font_small, c_x + 66, y + 32, dnd ? "On \xE2\x80\x94 banners are silenced" : "Off", ui_theme.dim, 255);
    dnd_x = c_x + c_w - 24 - 46; dnd_y = y + 18;
    switch_draw(s, dnd_x, dnd_y, dnd);

    /* the notifications, newest first */
    y += 76;
    int count = 0;
    for (int i = 0; i < NMAX; i++) if (ns[i].used && !ns[i].gone) count++;
    gfx_text(s, &ui_font_medium, c_x + 20, y, "Notifications", ui_theme.text, 255);
    clear_x = clear_y = -1000;
    if (count) {
        int tw = gfx_text_width(&ui_font_small, "Clear all");
        clear_x = c_x + c_w - 20 - tw - 16; clear_y = y - 3;
        gfx_rrect(s, clear_x, clear_y, tw + 16, 22, 11, ui_theme.dark ? 0x3A3B42 : 0xDCDCE2, 255);
        gfx_text(s, &ui_font_small, clear_x + 8, clear_y + 3, "Clear all", ui_theme.text, 255);
    }
    y += 30;
    for (int i = 0; i < NMAX; i++) ns[i].on_screen = 0;
    if (!count) {
        gfx_text_center(s, &ui_font_regular, c_x, y + 60, c_w, "No Notifications", ui_theme.dim, 255);
        return;
    }
    int shown = 0, left = 0;
    for (;;) {
        notif_t *best = 0;
        for (int i = 0; i < NMAX; i++)
            if (ns[i].used && !ns[i].gone && !ns[i].on_screen && (!best || ns[i].at > best->at || (ns[i].at == best->at && ns[i].id > best->id))) best = &ns[i];
        if (!best) break;
        int h = card_height(best, c_w - 24);
        if (y + h > c_y + c_h - 12) { left++; best->on_screen = 2; continue; }
        draw_card(s, best, c_x + 12, y, c_w - 24, 255);
        y += h + 10;
        shown++;
    }
    for (int i = 0; i < NMAX; i++) if (ns[i].on_screen == 2) ns[i].on_screen = 0;
    if (left) {
        char m[32] = "+";
        char d[8]; int o = 0, v = left;
        char r[8]; int ro = 0;
        do { r[ro++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (ro) d[o++] = r[--ro];
        d[o] = 0;
        strcat(m, d); strcat(m, " more");
        gfx_text_center(s, &ui_font_small, c_x, c_y + c_h - 24, c_w, m, ui_theme.dim, 255);
    }
    (void)shown;
}

void notify_draw(gfx_surface_t *s, int sw, int sh, int topbar_h)
{
    for (int i = 0; i < NMAX; i++) ns[i].on_screen = 0;
    if (center_open) { draw_center(s, sw, sh, topbar_h); return; }
    uint32_t now = debugmon_uptime_ms();
    int y = topbar_h + 10, shown = 0;
    /* newest on top */
    for (int pass = 0; pass < MAX_BANNERS; pass++) {
        notif_t *best = 0;
        for (int i = 0; i < NMAX; i++)
            if (ns[i].used && ns[i].banner && !ns[i].on_screen && (!best || ns[i].id > best->id)) best = &ns[i];
        if (!best) break;
        uint32_t dt = now - best->at;
        int slide = dt < SLIDE_MS ? (int)((BANNER_W + 30) * (SLIDE_MS - dt) / SLIDE_MS) : 0;
        draw_card(s, best, sw - BANNER_W - 12 + slide, y, BANNER_W, 246);
        y += best->bh + 10;
        shown++;
    }
    (void)shown;
}

static int in(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }

int notify_press(int x, int y, char *open_app, int open_app_len)
{
    if (open_app && open_app_len) open_app[0] = 0;
    for (int i = 0; i < NMAX; i++) {
        notif_t *n = &ns[i];
        if (!n->used || !n->on_screen || !in(x, y, n->bx, n->by, n->bw, n->bh)) continue;
        sound_click();
        if (in(x, y, n->bx + n->bw - 30, n->by + 6, 26, 26)) { close_notif(n, -1); return 1; }
        if (n->act[0][0] && !n->result) {
            int na = n->act[1][0] ? 2 : 1, by = n->by + n->bh - 40, bw = (n->bw - 64 - 14 - (na - 1) * 8) / na;
            for (int a = 0; a < na; a++)
                if (in(x, y, n->bx + 64 + a * (bw + 8), by, bw, 28)) { n->result = a + 1; close_notif(n, a + 1); return 1; }
        }
        /* the notification itself: it opens its app */
        close_notif(n, 3);
        if (open_app && uiapp_find(n->app)) copy(open_app, n->app, open_app_len);
        if (center_open) center_open = 0;
        return 1;
    }
    if (center_open) {
        if (in(x, y, dnd_x - 4, dnd_y - 4, 46, 30)) { notify_set_dnd(!dnd); sound_click(); return 1; }
        if (in(x, y, clear_x, clear_y, 120, 22)) {
            for (int i = 0; i < NMAX; i++) if (ns[i].used && !ns[i].gone) close_notif(&ns[i], -1);
            sound_click();
            return 1;
        }
        if (in(x, y, c_x, c_y, c_w, c_h)) return 1;
        center_open = 0;            /* a click anywhere else closes it, and goes on to what is there */
        return 0;
    }
    return 0;
}

int notify_tick(void)
{
    uint32_t now = debugmon_uptime_ms();
    int repaint = 0;
    for (int i = 0; i < NMAX; i++) {
        notif_t *n = &ns[i];
        if (!n->used || !n->banner) continue;
        uint32_t dt = now - n->at;
        if (dt < SLIDE_MS + 40) repaint = 1;
        if (!n->act[0][0] && dt > BANNER_MS) { n->banner = 0; repaint = 1; }
    }
    if (center_open && now - center_at < SLIDE_MS + 40) repaint = 1;
    /* the "now" / "5m ago" labels move on once a minute */
    if (now - last_anim > 30000) { last_anim = now; for (int i = 0; i < NMAX; i++) if (ns[i].used && ns[i].on_screen) repaint = 1; }
    return repaint;
}
