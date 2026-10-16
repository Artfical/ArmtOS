#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"

/*
 * System Settings, laid out like macOS's: the account at the top of a sidebar, sections with
 * coloured icons below, and on the right grouped rows with switches and segmented controls.
 */

enum { S_NETWORK, S_APPEARANCE, S_DESKTOP, S_DISPLAYS, S_MOUSE, S_USERS, S_GENERAL, S_COUNT };
static const char *const sec_name[S_COUNT] = { "Network", "Appearance", "Desktop & Dock", "Displays", "Mouse", "Users & Groups", "General" };
static const char *const sec_glyph[S_COUNT] = { "glyph_ethernet-port", "glyph_sun", "glyph_layout-panel-top", "glyph_monitor",
                                                "glyph_mouse-pointer", "glyph_users", "glyph_info" };
static const uint32_t sec_color[S_COUNT] = { 0x2F80ED, 0x1F1F24, 0x14B8A6, 0x3B82F6, 0x8E8E93, 0x8E8E93, 0x8E8E93 };

#define SIDE 230

typedef struct {
    int sec;
    uihost_user_t users[12];
    int nusers;
    /* the add-user form */
    int adding, field, admin;
    char new_name[24], new_full[40], new_pw[32];
    int pending;                  /* waiting for the password dialog: 1 add user */
    char msg[80];
    uint32_t last_sec;
} set_t;

/* ---- controls ---- */

static void sw(gfx_surface_t *s, int x, int y, int on)
{
    gfx_rrect(s, x, y, 40, 24, 12, on ? 0x34C759 : (ui_theme.dark ? 0x4A4B54 : 0xD9D9DE), 255);
    gfx_circle(s, on ? x + 28 : x + 12, y + 12, 10, 0xFFFFFF, 255);
}

static void segmented(gfx_surface_t *s, int x, int y, int w, const char *const *labels, int n, int sel)
{
    gfx_rrect(s, x, y, w, 28, 7, UI_CARD2, 255);
    int bw = w / n;
    gfx_rrect(s, x + 2 + sel * bw, y + 2, bw - 4, 24, 6, ui_theme.dark ? 0x5A5C66 : 0xFFFFFF, 255);
    for (int i = 0; i < n; i++) gfx_text_center(s, &ui_font_regular, x + i * bw, y + 5, bw, labels[i], UI_TEXT, 255);
}

static int group_top;             /* where the current grouped box began */

static void group(gfx_surface_t *s, int x, int y, int w, int rows)
{
    gfx_rrect(s, x, y, w, rows * 44, 10, UI_CARD, 255);
    for (int i = 1; i < rows; i++) gfx_hline(s, x + 14, y + i * 44, w - 28, UI_LINE, 255);
    group_top = y;
}

static void row_label(gfx_surface_t *s, int x, int y, int i, const char *label, const char *unused)
{
    (void)unused;
    gfx_text(s, &ui_font_regular, x + 14, y + i * 44 + 13, label, UI_TEXT, 255);
}

static void row_value(gfx_surface_t *s, int x, int w, int y, int i, const char *value)
{
    gfx_text(s, &ui_font_regular, x + w - 14 - gfx_text_width(&ui_font_regular, value), y + i * 44 + 13, value, UI_DIM, 255);
}

static void put_ip(char *o, uint32_t ip) { for (int i = 0; i < 4; i++) { ui_num(o, (ip >> (8 * i)) & 255); if (i < 3) strcat(o, "."); } }

static void avatar(gfx_surface_t *s, int cx, int cy, int r, const char *name)
{
    gfx_circle(s, cx, cy, r, 0x8E8E93, 255);
    char in[2] = { name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0], 0 };
    const ui_font_t *f = r > 20 ? &ui_font_title : &ui_font_medium;
    gfx_text_center(s, f, cx - r, cy - f->line_h / 2 + 1, r * 2, in, 0xFFFFFF, 255);
}

/* ---- sections ---- */

static void draw_section(uiapp_t *a, set_t *st, gfx_surface_t *s, int x, int y, int w)
{
    int px = x + 28, pw = w - 56;
    gfx_text(s, &ui_font_title, px, y + 18, sec_name[st->sec], UI_TEXT, 255);
    int gy = y + 64;
    char v[64];
    switch (st->sec) {
    case S_NETWORK: {
        uihost_net_t n;
        uihost_net(&n);
        group(s, px, gy, pw, 1);
        gfx_circle(s, px + 26, gy + 22, 12, 0x2F80ED, 255);
        gfx_blit(s, gfx_image("glyph_ethernet-port"), px + 18, gy + 14, 255);
        gfx_text(s, &ui_font_medium, px + 48, gy + 6, "Ethernet", UI_TEXT, 255);
        gfx_circle(s, px + 52, gy + 32, 4, n.up && n.ip ? 0x34C759 : 0x8E8E93, 255);
        gfx_text(s, &ui_font_small, px + 62, gy + 25, n.up && n.ip ? "Connected" : n.up ? "No address" : "Not connected", UI_DIM, 255);
        ui_button(s, px + pw - 110, gy + 8, 96, 28, "Details...", UI_CARD2, UI_TEXT, 0);
        gy += 64;
        group(s, px, gy, pw, 5);
        const char *lab[5] = { "IP Address", "Subnet Mask", "Router", "DNS Server", "MAC Address" };
        uint32_t vals[4] = { n.ip, n.netmask, n.gateway, n.dns };
        for (int i = 0; i < 5; i++) {
            row_label(s, px, gy, i, lab[i], 0);
            v[0] = 0;
            if (i < 4) { if (n.up) put_ip(v, vals[i]); else strcpy(v, "-"); }
            else for (int k = 0; k < 6; k++) { char c[4] = { "0123456789ABCDEF"[n.mac[k] >> 4], "0123456789ABCDEF"[n.mac[k] & 15], k < 5 ? ':' : 0, 0 }; strcat(v, c); }
            row_value(s, px, pw, gy, i, v);
        }
        break;
    }
    case S_APPEARANCE: {
        group(s, px, gy, pw, 3);
        row_label(s, px, gy, 0, "Appearance", 0);
        /* the two previews */
        int dark = uihost_get("dark");
        static const char *const ap[2] = { "Light", "Dark" };
        segmented(s, px + pw - 214, gy + 8, 200, ap, 2, dark);
        row_label(s, px, gy, 1, "Accent colour", 0);
        static const uint32_t acc[7] = { 0x3B82F6, 0xA855F7, 0xEC4899, 0xEF4444, 0xF97316, 0xEAB308, 0x22C55E };
        for (int k = 0; k < 7; k++) gfx_circle(s, px + pw - 190 + k * 26, gy + 44 + 22, 8, acc[k], 255);
        gfx_rrect_outline(s, px + pw - 190 - 11, gy + 44 + 11, 22, 22, 11, UI_TEXT, 200);
        row_label(s, px, gy, 2, "Window shadows", 0);
        sw(s, px + pw - 54, gy + 88 + 10, 1);
        /* the previews */
        int ty = gy + 150;
        for (int k = 0; k < 2; k++) {
            int bx = px + k * 170;
            gfx_rrect(s, bx - 3, ty - 3, 156, 102, 12, dark == k ? UI_ACCENT : UI_BG, 255);
            gfx_rrect(s, bx, ty, 150, 96, 10, k ? 0x1E1F25 : 0xECECEF, 255);
            gfx_rrect(s, bx + 14, ty + 14, 122, 68, 7, k ? 0x2C2E37 : 0xFFFFFF, 255);
            gfx_rrect(s, bx + 14, ty + 14, 122, 14, 7, k ? 0x3A3C46 : 0xE3E3E8, 255);
            for (int c = 0; c < 3; c++) gfx_circle(s, bx + 24 + c * 10, ty + 21, 3, c == 0 ? 0xFF5F57 : c == 1 ? 0xFEBC2E : 0x28C840, 255);
            gfx_text_center(s, &ui_font_regular, bx, ty + 104, 150, ap[k], UI_TEXT, 255);
        }
        break;
    }
    case S_DESKTOP: {
        int wp = uihost_get("wallpaper");
        static const char *wn[4] = { "Dusk", "Ocean", "Graphite", "Picture..." };
        static const uint32_t wa[3][2] = { { 0x302468, 0xE07A6E }, { 0x0B3A66, 0x2BB3C9 }, { 0x2A2C33, 0x5A5F6B } };
        gfx_text(s, &ui_font_small, px, gy - 4, "WALLPAPER", UI_DIM, 255);
        for (int i = 0; i < 4; i++) {
            int bx = px + i * 128, by = gy + 16;
            gfx_rrect(s, bx - 3, by - 3, 118, 72, 11, wp == i ? UI_ACCENT : UI_BG, 255);
            gfx_surface_t t = *s;
            gfx_clip(&t, bx, by, 112, 66);
            if (i < 3) gfx_gradient_v(&t, bx, by, 112, 66, wa[i][0], wa[i][1]);
            else { gfx_fill(&t, bx, by, 112, 66, UI_CARD2); gfx_blit(s, gfx_image("doc64_image"), bx + 24, by + 1, 255); }
            gfx_text_center(s, &ui_font_small, bx, by + 74, 112, wn[i], UI_TEXT, 255);
        }
        gy += 120;
        group(s, px, gy, pw, 2);
        row_label(s, px, gy, 0, "Magnification", 0);
        sw(s, px + pw - 54, gy + 10, uihost_get("dock_magnify"));
        row_label(s, px, gy, 1, "Show indicators for open apps", 0);
        sw(s, px + pw - 54, gy + 44 + 10, 1);
        break;
    }
    case S_DISPLAYS: {
        static const int rw[5] = { 1024, 1280, 1366, 1600, 1920 }, rh[5] = { 768, 720, 768, 900, 1080 };
        int cw = uihost_get("width"), ch = uihost_get("height");
        /* a picture of the display */
        gfx_rrect(s, px + pw / 2 - 90, gy, 180, 110, 8, 0x111114, 255);
        gfx_rrect(s, px + pw / 2 - 84, gy + 6, 168, 98, 5, 0x3B5BDB, 255);
        gfx_fill(s, px + pw / 2 - 12, gy + 110, 24, 14, 0x9A9AA0);
        gfx_rrect(s, px + pw / 2 - 40, gy + 124, 80, 6, 3, 0x9A9AA0, 255);
        v[0] = 0; ui_num(v, cw); strcat(v, " x "); ui_num(v, ch);
        gfx_text_center(s, &ui_font_medium, px, gy + 140, pw, v, UI_TEXT, 255);
        gy += 176;
        group(s, px, gy, pw, 5);
        for (int i = 0; i < 5; i++) {
            v[0] = 0; ui_num(v, rw[i]); strcat(v, " x "); ui_num(v, rh[i]);
            if (i == 1) strcat(v, "  (default)");
            row_label(s, px, gy, i, v, 0);
            if (cw == rw[i] && ch == rh[i]) gfx_text(s, &ui_font_medium, px + pw - 30, gy + i * 44 + 12, "\xE2\x9C\x93", UI_ACCENT, 255);
        }
        break;
    }
    case S_MOUSE: {
        group(s, px, gy, pw, 1);
        row_label(s, px, gy, 0, "Tracking speed", 0);
        static const char *const sp[3] = { "Slow", "Normal", "Fast" };
        segmented(s, px + pw - 254, gy + 8, 240, sp, 3, uihost_get("mouse_speed"));
        break;
    }
    case S_USERS: {
        st->nusers = uihost_users(st->users, 12);
        group(s, px, gy, pw, st->nusers ? st->nusers : 1);
        for (int i = 0; i < st->nusers; i++) {
            avatar(s, px + 30, gy + i * 44 + 22, 15, st->users[i].name);
            gfx_text(s, &ui_font_medium, px + 56, gy + i * 44 + 5, st->users[i].full, UI_TEXT, 255);
            gfx_text(s, &ui_font_small, px + 56, gy + i * 44 + 24, st->users[i].uid == 0 ? "System administrator" : st->users[i].admin ? "Admin" : "Standard", UI_DIM, 255);
            if (st->users[i].current) row_value(s, px, pw, gy, i, "You");
        }
        gy += (st->nusers ? st->nusers : 1) * 44 + 16;
        if (!st->adding) ui_button(s, px + pw - 130, gy, 130, 30, "Add User...", UI_CARD2, UI_TEXT, 0);
        else {
            group(s, px, gy, pw, 4);
            const char *lab[3] = { "Full name", "Account name", "Password" };
            char *val[3] = { st->new_full, st->new_name, st->new_pw };
            for (int i = 0; i < 3; i++) {
                row_label(s, px, gy, i, lab[i], 0);
                int fx = px + 160, fw = pw - 174;
                gfx_rrect(s, fx, gy + i * 44 + 8, fw, 28, 6, UI_FIELD, 255);
                if (st->field == i) gfx_rrect_outline(s, fx - 1, gy + i * 44 + 7, fw + 2, 30, 7, UI_ACCENT, 255);
                if (i == 2) { for (int k = 0; k < (int)strlen(val[i]) && k < 30; k++) gfx_circle(s, fx + 14 + k * 11, gy + i * 44 + 22, 3, UI_TEXT, 255); }
                else gfx_text(s, &ui_font_regular, fx + 10, gy + i * 44 + 13, val[i], UI_TEXT, 255);
            }
            row_label(s, px, gy, 3, "Allow this user to administer the computer", 0);
            sw(s, px + pw - 54, gy + 3 * 44 + 10, st->admin);
            gy += 4 * 44 + 12;
            ui_button(s, px + pw - 236, gy, 100, 30, "Cancel", UI_CARD2, UI_TEXT, 0);
            ui_button(s, px + pw - 126, gy, 126, 30, "Create User", UI_ACCENT, 0xFFFFFF, 0);
        }
        if (st->msg[0]) gfx_text(s, &ui_font_small, px, gy + 8, st->msg, UI_DIM, 255);
        break;
    }
    case S_GENERAL: {
        gfx_blit_scaled(s, gfx_image("icon48_launcher"), px + pw / 2 - 36, gy, 72, 72, 255);
        gfx_text_center(s, &ui_font_huge, px, gy + 76, pw, uihost_system_name(), UI_TEXT, 255);
        gy += 150;
        group(s, px, gy, pw, 4);
        uint32_t tot, used;
        uihost_memory(&tot, &used);
        row_label(s, px, gy, 0, "Name", 0);         row_value(s, px, pw, gy, 0, uihost_hostname());
        v[0] = 0; ui_size(v, tot * 1024); strcat(v, " ("); ui_size(v, used * 1024); strcat(v, " in use)");
        row_label(s, px, gy, 1, "Memory", 0);       row_value(s, px, pw, gy, 1, v);
        v[0] = 0; ui_num(v, uihost_get("width")); strcat(v, " x "); ui_num(v, uihost_get("height"));
        row_label(s, px, gy, 2, "Display", 0);      row_value(s, px, pw, gy, 2, v);
        uint32_t up = uihost_ms() / 1000;
        v[0] = 0; ui_num(v, up / 3600); strcat(v, " h "); ui_num(v, up / 60 % 60); strcat(v, " min");
        row_label(s, px, gy, 3, "Uptime", 0);       row_value(s, px, pw, gy, 3, v);
        a->dirty = 0;
        break;
    }
    }
}

static void s_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    set_t *st = a->st;
    /* the sidebar */
    gfx_fill(s, x, y, SIDE, h, UI_SIDEBAR);
    gfx_rrect(s, x + 12, y + 12, SIDE - 24, 28, 7, ui_theme.dark ? 0x2E2F37 : 0xDEDEE3, 255);
    gfx_blit_tinted(s, gfx_image("glyph_search"), x + 20, y + 18, UI_DIM, 255);
    gfx_text(s, &ui_font_regular, x + 42, y + 17, "Search", UI_DIM, 255);
    uihost_user_t me = { "user", "User", 0, 0, 1 };
    uihost_user_t us[12];
    int nu = uihost_users(us, 12);
    for (int i = 0; i < nu; i++) if (us[i].current) me = us[i];
    avatar(s, x + 36, y + 76, 22, me.name);
    gfx_text(s, &ui_font_medium, x + 68, y + 60, me.full, UI_TEXT, 255);
    gfx_text(s, &ui_font_small, x + 68, y + 80, "Local account", UI_DIM, 255);
    for (int i = 0; i < S_COUNT; i++) {
        int ry = y + 116 + i * 34;
        if (i == st->sec) gfx_rrect(s, x + 8, ry, SIDE - 16, 30, 7, UI_ACCENT, 255);
        gfx_rrect(s, x + 16, ry + 4, 22, 22, 6, sec_color[i], 255);
        const gfx_image_t *g = gfx_image(sec_glyph[i]);
        if (g) gfx_blit_scaled(s, g, x + 19, ry + 7, 16, 16, 255);
        gfx_text(s, &ui_font_regular, x + 48, ry + 6, sec_name[i], i == st->sec ? 0xFFFFFF : UI_TEXT, 255);
    }
    gfx_fill(s, x + SIDE, y, 1, h, UI_LINE);
    gfx_fill(s, x + SIDE + 1, y, w - SIDE - 1, h, UI_BG);
    gfx_surface_t c = *s;
    gfx_clip(&c, x + SIDE + 1, y, w - SIDE - 1, h);
    draw_section(a, st, &c, x + SIDE + 1, y, w - SIDE - 1);
}

static void s_init(uiapp_t *a, const char *arg)
{
    set_t *st = a->st;
    st->sec = arg && !strcmp(arg, "network") ? S_NETWORK : S_APPEARANCE;
}

static void s_press(uiapp_t *a, int lx, int ly, int b)
{
    (void)b;
    set_t *st = a->st;
    a->dirty = 1;
    if (lx < SIDE) {
        int i = (ly - 116) / 34;
        if (ly >= 116 && i >= 0 && i < S_COUNT) { st->sec = i; st->adding = 0; st->msg[0] = 0; }
        return;
    }
    int px = SIDE + 1 + 28, pw = a->w - SIDE - 1 - 56, gy = 64;
    lx -= 0;
    switch (st->sec) {
    case S_NETWORK:
        if (ui_hit(lx, ly, px + pw - 110, gy + 8, 96, 28)) uihost_open("netmon", 0);
        break;
    case S_APPEARANCE:
        if (ui_hit(lx, ly, px + pw - 214, gy + 8, 200, 28)) uihost_set("dark", lx >= px + pw - 114);
        for (int k = 0; k < 2; k++) if (ui_hit(lx, ly, px + k * 170, gy + 150, 150, 96)) uihost_set("dark", k);
        break;
    case S_DESKTOP:
        for (int i = 0; i < 4; i++)
            if (ui_hit(lx, ly, px + i * 128, gy + 16, 112, 66)) {
                if (i < 3) uihost_set("wallpaper", i);
                else uihost_open("files", 0);              /* pick a picture there: "Wallpaper" */
            }
        if (ui_hit(lx, ly, px + pw - 54, gy + 120 + 10, 40, 24)) uihost_set("dock_magnify", !uihost_get("dock_magnify"));
        break;
    case S_DISPLAYS: {
        int row = (ly - (gy + 176)) / 44;
        if (ly >= gy + 176 && row >= 0 && row < 5) uihost_set("resolution", row);
        break;
    }
    case S_MOUSE:
        if (ui_hit(lx, ly, px + pw - 254, gy + 8, 240, 28)) uihost_set("mouse_speed", (lx - (px + pw - 254)) / 80);
        break;
    case S_USERS: {
        int top = gy + (st->nusers ? st->nusers : 1) * 44 + 16;
        if (!st->adding) {
            if (ui_hit(lx, ly, px + pw - 130, top, 130, 30)) {
                st->adding = 1; st->field = 0; st->admin = 0;
                st->new_name[0] = st->new_full[0] = st->new_pw[0] = 0; st->msg[0] = 0;
            }
        } else {
            for (int i = 0; i < 3; i++) if (ui_hit(lx, ly, px + 160, top + i * 44 + 8, pw - 174, 28)) st->field = i;
            if (ui_hit(lx, ly, px + pw - 54, top + 3 * 44 + 10, 40, 24)) st->admin = !st->admin;
            int by = top + 4 * 44 + 12;
            if (ui_hit(lx, ly, px + pw - 236, by, 100, 30)) st->adding = 0;
            else if (ui_hit(lx, ly, px + pw - 126, by, 126, 30)) {
                if (!st->new_name[0] || !st->new_pw[0]) strcpy(st->msg, "An account name and a password are needed.");
                else { st->pending = 1; uihost_authorize("add a user"); }
            }
        }
        break;
    }
    }
}

static void s_key(uiapp_t *a, int k)
{
    set_t *st = a->st;
    if (st->sec == S_USERS && st->adding) {
        char *f = st->field == 0 ? st->new_full : st->field == 1 ? st->new_name : st->new_pw;
        int cap = st->field == 0 ? 39 : st->field == 1 ? 23 : 31;
        int n = (int)strlen(f);
        if (k == '\t' || k == '\n') st->field = (st->field + 1) % 3;
        else if (k == '\b') { if (n) f[n - 1] = 0; }
        else if (k > 32 && k < 127 && n < cap) { f[n] = (char)(st->field == 1 && k >= 'A' && k <= 'Z' ? k + 32 : k); f[n + 1] = 0; }
        else if (k == ' ' && st->field == 0 && n < cap) { f[n] = ' '; f[n + 1] = 0; }
    } else if (k == UI_KEY_DOWN && st->sec + 1 < S_COUNT) st->sec++;
    else if (k == UI_KEY_UP && st->sec > 0) st->sec--;
    a->dirty = 1;
}

static int s_tick(uiapp_t *a)
{
    set_t *st = a->st;
    if (st->pending) {
        int r = uihost_authorized();
        if (r == 0) return 0;
        if (r > 0) {
            int rc = uihost_add_user(st->new_name, st->new_pw, st->new_full[0] ? st->new_full : st->new_name, st->admin);
            strcpy(st->msg, rc == 0 ? "User created." : "Could not create that user (is the name taken?).");
            if (rc == 0) st->adding = 0;
        } else strcpy(st->msg, "Not authorised.");
        memset(st->new_pw, 0, sizeof(st->new_pw));
        st->pending = 0;
        return 1;
    }
    if (st->sec == S_GENERAL || st->sec == S_NETWORK) {
        uint32_t sec = uihost_ms() / 1000;
        if (sec != st->last_sec) { st->last_sec = sec; return 1; }
    }
    return 0;
}

const uiapp_class_t app_settings = {
    "settings", "System Settings", "settings", 820, 540, 0, sizeof(set_t),
    s_init, s_draw, s_press, 0, 0, s_key, 0, s_tick, 0,
};
