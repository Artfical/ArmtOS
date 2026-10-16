#include "../uiapp.h"
#include "string.h"

/*
 * Calculator. Numbers are fixed point with six decimals in a 64-bit integer, because the ARM
 * kernel has no floating point -- enough for twelve digits before the point.
 */

#define SCALE 1000000LL

typedef struct {
    int64_t acc, cur;            /* the stored value and the one being typed */
    int op;                      /* pending operator: + - * / or 0 */
    int typing;                  /* digits are being entered into cur */
    int decimals;                /* digits typed after the point, -1 = no point yet */
    int error;
    int hot;                     /* index of the button under the pointer when pressed */
} calc_t;

static const char *const keys[5][4] = {
    { "AC", "+/-", "%", "/" },
    { "7", "8", "9", "*" },
    { "4", "5", "6", "-" },
    { "1", "2", "3", "+" },
    { "0", "", ".", "=" },
};

static int64_t mul(int64_t a, int64_t b, int *err)
{
    int neg = (a < 0) != (b < 0);
    uint64_t x = (uint64_t)(a < 0 ? -a : a), y = (uint64_t)(b < 0 ? -b : b);
    uint64_t hi = x / SCALE, lo = x % SCALE;
    if (hi && y > 0x7FFFFFFFFFFFFFFFULL / hi) { *err = 1; return 0; }
    uint64_t r = hi * y + lo * y / SCALE;
    return neg ? -(int64_t)r : (int64_t)r;
}

static int64_t divi(int64_t a, int64_t b, int *err)
{
    if (b == 0) { *err = 1; return 0; }
    int neg = (a < 0) != (b < 0);
    uint64_t x = (uint64_t)(a < 0 ? -a : a), y = (uint64_t)(b < 0 ? -b : b);
    uint64_t q = x / y, r = x % y;
    if (q > 0x7FFFFFFFFFFFFFFFULL / SCALE) { *err = 1; return 0; }
    q *= SCALE;
    for (int64_t place = SCALE / 10; place > 0; place /= 10) {   /* long division, one digit at a time */
        r *= 10;
        q += (r / y) * (uint64_t)place;
        r %= y;
    }
    return neg ? -(int64_t)q : (int64_t)q;
}

static void apply(calc_t *c)
{
    int err = 0;
    switch (c->op) {
    case '+': c->acc += c->cur; break;
    case '-': c->acc -= c->cur; break;
    case '*': c->acc = mul(c->acc, c->cur, &err); break;
    case '/': c->acc = divi(c->acc, c->cur, &err); break;
    default:  c->acc = c->cur; break;
    }
    if (err || c->acc > 999999999999LL * SCALE || c->acc < -999999999999LL * SCALE) c->error = 1;
}

static void press_key(calc_t *c, const char *k)
{
    if (!k[0]) return;
    if (c->error && strcmp(k, "AC")) return;
    if (k[0] >= '0' && k[0] <= '9') {
        if (!c->typing) { c->cur = 0; c->typing = 1; c->decimals = -1; }
        int d = k[0] - '0';
        if (c->decimals < 0) {
            if (c->cur < 99999999999LL * SCALE) c->cur = (c->cur / SCALE * 10 + d) * SCALE;
        } else if (c->decimals < 6) {
            int64_t place = SCALE;
            for (int i = 0; i <= c->decimals; i++) place /= 10;
            c->cur += (c->cur < 0 ? -d : d) * place;
            c->decimals++;
        }
    } else if (!strcmp(k, ".")) {
        if (!c->typing) { c->cur = 0; c->typing = 1; }
        if (c->decimals < 0) c->decimals = 0;
    } else if (!strcmp(k, "AC")) {
        memset(c, 0, sizeof(*c));
        c->decimals = -1;
    } else if (!strcmp(k, "+/-")) {
        c->cur = -c->cur;
    } else if (!strcmp(k, "%")) {
        c->cur /= 100;
    } else if (!strcmp(k, "=")) {
        apply(c);
        c->cur = c->acc;
        c->op = 0;
        c->typing = 0;
    } else {                                   /* an operator */
        if (c->typing || !c->op) apply(c);
        c->op = k[0];
        c->cur = c->acc;
        c->typing = 0;
    }
}

static void format(int64_t v, char *out)
{
    out[0] = 0;
    if (v < 0) { strcpy(out, "-"); v = -v; }
    int64_t ip = v / SCALE, fp = v % SCALE;
    /* thousands separators for the whole part */
    char digits[24];
    int n = 0;
    if (!ip) digits[n++] = '0';
    while (ip) { digits[n++] = (char)('0' + ip % 10); ip /= 10; }
    int o = (int)strlen(out);
    for (int i = n - 1; i >= 0; i--) {
        out[o++] = digits[i];
        if (i && i % 3 == 0) out[o++] = ',';
    }
    out[o] = 0;
    if (fp) {
        char f[8];
        for (int i = 5; i >= 0; i--) { f[i] = (char)('0' + fp % 10); fp /= 10; }
        int len = 6;
        while (len > 0 && f[len - 1] == '0') len--;
        out[o++] = '.';
        for (int i = 0; i < len; i++) out[o++] = f[i];
        out[o] = 0;
    }
}

static void calc_init(uiapp_t *a, const char *arg) { (void)arg; ((calc_t *)a->st)->decimals = -1; ((calc_t *)a->st)->hot = -1; }

#define BTN 62
#define GAP 10
#define TOPH 120

static void key_rect(int r, int col, int *x, int *y, int *w, int *h)
{
    *x = GAP + col * (BTN + GAP);
    *y = TOPH + r * (BTN + GAP);
    *w = (r == 4 && col == 0) ? BTN * 2 + GAP : BTN;
    *h = BTN;
}

static void calc_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    calc_t *c = a->st;
    gfx_fill(s, x, y, w, h, 0x18191E);
    char shown[40];
    if (c->error) strcpy(shown, "Error");
    else format(c->typing ? c->cur : (c->op ? c->acc : c->cur), shown);
    const ui_font_t *f = gfx_text_width(&ui_font_huge, shown) < w - 30 ? &ui_font_huge : &ui_font_title;
    gfx_text(s, f, x + w - 16 - gfx_text_width(f, shown), y + TOPH - f->line_h - 8, shown, UI_TEXT, 255);
    if (c->op) {
        char o[2] = { (char)c->op, 0 };
        gfx_text(s, &ui_font_medium, x + 18, y + 18, o, UI_ORANGE, 255);
    }
    for (int r = 0; r < 5; r++)
        for (int col = 0; col < 4; col++) {
            const char *k = keys[r][col];
            if (!k[0]) continue;
            int bx, by, bw, bh;
            key_rect(r, col, &bx, &by, &bw, &bh);
            int is_op = col == 3, is_fn = r == 0 && col < 3;
            uint32_t bg = is_op ? UI_ORANGE : is_fn ? 0x5A5C66 : 0x33343D;
            if (is_op && c->op == k[0] && !c->typing) bg = 0xFFD9A0;
            if (c->hot == r * 4 + col) bg = gfx_mix(bg, 0xFFFFFF, 70);
            gfx_rrect(s, x + bx, y + by, bw, bh, BTN / 2, bg, 255);
            const char *label = !strcmp(k, "*") ? "\xC3\x97" : !strcmp(k, "/") ? "\xC3\xB7" : k;   /* × ÷ */
            uint32_t fg = (is_op && c->op == k[0] && !c->typing) ? UI_ORANGE : UI_TEXT;
            if (r == 4 && col == 0) gfx_text(s, &ui_font_title, x + bx + 24, y + by + (bh - 30) / 2, label, fg, 255);
            else gfx_text_center(s, &ui_font_title, x + bx, y + by + (bh - 30) / 2, bw, label, fg, 255);
        }
}

static void calc_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    calc_t *c = a->st;
    for (int r = 0; r < 5; r++)
        for (int col = 0; col < 4; col++) {
            int bx, by, bw, bh;
            key_rect(r, col, &bx, &by, &bw, &bh);
            if (keys[r][col][0] && ui_hit(px, py, bx, by, bw, bh)) {
                c->hot = r * 4 + col;
                press_key(c, keys[r][col]);
                a->dirty = 1;
                return;
            }
        }
}

static void calc_release(uiapp_t *a, int x, int y) { (void)x; (void)y; ((calc_t *)a->st)->hot = -1; a->dirty = 1; }

static void calc_key(uiapp_t *a, int k)
{
    calc_t *c = a->st;
    char s[2] = { (char)k, 0 };
    if ((k >= '0' && k <= '9') || k == '.' || k == '+' || k == '-' || k == '*' || k == '/' || k == '%') press_key(c, s);
    else if (k == '\n' || k == '=') press_key(c, "=");
    else if (k == UI_KEY_ESC || k == 'c' || k == 'C') press_key(c, "AC");
    else if (k == '\b' && c->typing) {
        /* drop the last digit typed */
        if (c->decimals > 0) {
            int64_t place = SCALE;
            for (int i = 0; i < c->decimals; i++) place /= 10;
            int64_t mag = c->cur < 0 ? -c->cur : c->cur;
            int64_t digit = (mag / place) % 10;
            c->cur += (c->cur < 0 ? digit : -digit) * place;
            c->decimals--;
        } else if (c->decimals == 0) c->decimals = -1;
        else c->cur = c->cur / SCALE / 10 * SCALE;
    }
    a->dirty = 1;
}

const uiapp_class_t app_calculator = {
    "calculator", "Calculator", "calculator", 4 * BTN + 5 * GAP, TOPH + 5 * BTN + 5 * GAP, 0, sizeof(calc_t),
    calc_init, calc_draw, calc_press, 0, calc_release, calc_key, 0, 0, 0,
};
