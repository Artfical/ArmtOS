#include "uiapp.h"
#include "string.h"

extern const uiapp_class_t app_calculator, app_clock, app_2048;

const uiapp_class_t *const uiapp_classes[] = {
    &app_calculator, &app_clock, &app_2048,
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
