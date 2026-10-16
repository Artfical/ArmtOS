#include "cfgfile.h"
#include "fat.h"
#include "emmc.h"
#include "string_arm.h"

static char buf[CFG_MAX + 1];
static char tmp[CFG_MAX + 1];
static int len;
static int have;                       /* a file was read, or an edit created one */
static int tried;

static int is_space(char c) { return c == ' ' || c == '\t'; }

int cfg_load(void)
{
    tried = 1;
    len = 0;
    buf[0] = 0;
    have = 0;
    if (emmc_init() != 0 || fat_mount() != 0) return -1;
    int n = fat_read_file("config.txt", (uint8_t *)buf, CFG_MAX);
    if (n < 0) return -1;
    len = n;
    buf[len] = 0;
    have = 1;
    return 0;
}

int cfg_loaded(void) { return have; }
const char *cfg_text(void) { return buf; }
int cfg_length(void) { return len; }

int cfg_save(void)
{
    if (emmc_init() != 0 || fat_mount() != 0) return -1;
    return fat_write_file("config.txt", (const uint8_t *)buf, (uint32_t)len) < 0 ? -1 : 0;
}

/* ---- line helpers ---- */

/* does the line at p (ending at e) match "^[#]?\s*key=" -- optional comment mark allowed? */
static int line_matches(const char *p, const char *e, const char *key, int allow_hash)
{
    if (allow_hash && p < e && *p == '#') p++;
    while (p < e && is_space(*p)) p++;
    size_t kl = strlen(key);
    if ((size_t)(e - p) < kl + 1) return 0;
    if (memcmp(p, key, kl) != 0) return 0;
    return p[kl] == '=';
}

typedef void (*line_fn)(const char *p, const char *e, void *ctx, char *out, int *o);

/* Rebuilds the buffer line by line. `fn` appends whatever it wants the line to become (including
 * its newline) to out, or nothing to drop the line. */
static void rewrite(line_fn fn, void *ctx)
{
    int o = 0;
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *e = p;
        while (e < end && *e != '\n') e++;
        const char *line_end = e;
        if (line_end > p && line_end[-1] == '\r') line_end--;
        int before = o;
        fn(p, line_end, ctx, tmp, &o);
        if (o > before && o < CFG_MAX) tmp[o++] = '\n';
        p = e < end ? e + 1 : e;
        if (o > CFG_MAX - 256) break;
    }
    memcpy(buf, tmp, (size_t)o);
    len = o;
    buf[len] = 0;
    have = 1;
}

static void put_line(const char *p, const char *e, char *out, int *o)
{
    int n = (int)(e - p);
    if (*o + n > CFG_MAX - 2) return;
    memcpy(out + *o, p, (size_t)n);
    *o += n;
}

static void put_str(const char *s, char *out, int *o)
{
    put_line(s, s + strlen(s), out, o);
}

/* ---- get / set / clear ---- */

int cfg_get(const char *key, char *outv, int max)
{
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *e = p;
        while (e < end && *e != '\n') e++;
        const char *le = e;
        if (le > p && le[-1] == '\r') le--;
        if (line_matches(p, le, key, 0)) {
            const char *v = p;
            while (is_space(*v)) v++;
            v += strlen(key) + 1;
            int n = (int)(le - v);
            if (n > max - 1) n = max - 1;
            memcpy(outv, v, (size_t)n);
            outv[n] = 0;
            return 1;
        }
        p = e < end ? e + 1 : e;
    }
    return 0;
}

struct kv { const char *key, *value; int matched; };

static void set_fn(const char *p, const char *e, void *c, char *out, int *o)
{
    struct kv *k = c;
    if (line_matches(p, e, k->key, 1)) {
        put_str(k->key, out, o); put_str("=", out, o); put_str(k->value, out, o);
        k->matched = 1;
    } else put_line(p, e, out, o);
}

void cfg_set(const char *key, const char *value)
{
    struct kv k = { key, value, 0 };
    rewrite(set_fn, &k);
    if (!k.matched) {
        char line[160];
        size_t a = strlen(key), b = strlen(value);
        if (a + b + 2 > sizeof(line)) return;
        memcpy(line, key, a); line[a] = '='; memcpy(line + a + 1, value, b); line[a + 1 + b] = 0;
        cfg_append_line(line);
    }
}

static void clear_fn(const char *p, const char *e, void *c, char *out, int *o)
{
    struct kv *k = c;
    if (line_matches(p, e, k->key, 0)) put_str("#", out, o);
    put_line(p, e, out, o);
}

void cfg_clear(const char *key)
{
    struct kv k = { key, 0, 0 };
    rewrite(clear_fn, &k);
}

/* ---- sed-style edits ---- */

struct pfx { const char *prefix; const char *repl; };

static int starts(const char *p, const char *e, const char *pre)
{
    size_t n = strlen(pre);
    return (size_t)(e - p) >= n && !memcmp(p, pre, n);
}

static void comment_fn(const char *p, const char *e, void *c, char *out, int *o)
{
    struct pfx *x = c;
    if (starts(p, e, x->prefix)) put_str("#", out, o);
    put_line(p, e, out, o);
}

void cfg_comment_prefix(const char *prefix) { struct pfx x = { prefix, 0 }; rewrite(comment_fn, &x); }

static void uncomment_fn(const char *p, const char *e, void *c, char *out, int *o)
{
    struct pfx *x = c;
    if (p < e && *p == '#' && starts(p + 1, e, x->prefix)) put_line(p + 1, e, out, o);
    else put_line(p, e, out, o);
}

void cfg_uncomment_prefix(const char *prefix) { struct pfx x = { prefix, 0 }; rewrite(uncomment_fn, &x); }

int cfg_has_prefix(const char *prefix)
{
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *e = p;
        while (e < end && *e != '\n') e++;
        if (starts(p, e, prefix)) return 1;
        p = e < end ? e + 1 : e;
    }
    return 0;
}

static void replace_fn(const char *p, const char *e, void *c, char *out, int *o)
{
    struct pfx *x = c;
    if (starts(p, e, x->prefix)) put_str(x->repl, out, o); else put_line(p, e, out, o);
}

void cfg_replace_prefix(const char *prefix, const char *line) { struct pfx x = { prefix, line }; rewrite(replace_fn, &x); }

static void delete_fn(const char *p, const char *e, void *c, char *out, int *o)
{
    struct pfx *x = c;
    size_t n = strlen(x->prefix);
    for (const char *q = p; q + n <= e; q++)
        if (!memcmp(q, x->prefix, n)) return;               /* drop the line */
    put_line(p, e, out, o);
}

void cfg_delete_containing(const char *text) { struct pfx x = { text, 0 }; rewrite(delete_fn, &x); }

void cfg_append_line(const char *line)
{
    size_t n = strlen(line);
    if (len + (int)n + 2 > CFG_MAX) return;
    if (len > 0 && buf[len - 1] != '\n') buf[len++] = '\n';      /* sed '$a' keeps lines separate */
    memcpy(buf + len, line, n);
    len += (int)n;
    buf[len++] = '\n';
    buf[len] = 0;
    have = 1;
}

/* ---- dtparam ---- */

static int item_matches(const char *s, const char *e, const char *name, int *value_on, int *has_value)
{
    size_t nl = strlen(name);
    if ((size_t)(e - s) < nl || memcmp(s, name, nl) != 0) return 0;
    if (s + nl == e) { *has_value = 0; *value_on = 1; return 1; }
    if (s[nl] != '=') return 0;
    const char *v = s + nl + 1;
    size_t vl = (size_t)(e - v);
    *has_value = 1;
    *value_on = (vl == 2 && !memcmp(v, "on", 2)) || (vl == 4 && !memcmp(v, "true", 4)) ||
                (vl == 3 && !memcmp(v, "yes", 3)) || (vl == 1 && v[0] == '1');
    return 1;
}

/* scans dtparam lines; mode 1 = some item enables `name`, mode 2 = some item mentions it with an off value */
static int dtparam_scan(const char *name, int mode)
{
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *e = p;
        while (e < end && *e != '\n') e++;
        const char *le = e;
        if (le > p && le[-1] == '\r') le--;
        const char *list = 0;
        if (starts(p, le, "dtparam=")) list = p + 8;
        else if (starts(p, le, "device_tree_param=")) list = p + 18;
        if (list) {
            const char *s = list;
            while (s <= le) {
                const char *c = s;
                while (c < le && *c != ',') c++;
                int on = 0, hv = 0;
                if (item_matches(s, c, name, &on, &hv)) {
                    if (mode == 1 && on) return 1;
                    if (mode == 2 && hv && !on) return 1;
                }
                s = c + 1;
            }
        }
        p = e < end ? e + 1 : e;
    }
    return 0;
}

int cfg_dtparam_on(const char *name) { return dtparam_scan(name, 1); }
int cfg_dtparam_off(const char *name) { return dtparam_scan(name, 2); }

/* ---- what the kernel honours ----
 * raspi-config's switches for I2C and SPI are real here: with a readable config.txt, the bus
 * is usable only if the file switches it on, exactly as on Raspberry Pi OS. Without a readable
 * card there is nothing to consult, so nothing is withheld. */
int bootcfg_interface_allowed(const char *name)
{
    if (!tried) cfg_load();
    if (!have) return 1;
    if (!strcmp(name, "i2c")) return cfg_dtparam_on("i2c_arm") || cfg_dtparam_on("i2c");
    if (!strcmp(name, "spi")) return cfg_dtparam_on("spi");
    return 1;
}

int bootcfg_framebuffer(uint32_t *w, uint32_t *h)
{
    if (!tried) cfg_load();
    char a[16], b[16];
    if (!have || !cfg_get("framebuffer_width", a, sizeof(a)) || !cfg_get("framebuffer_height", b, sizeof(b))) return 0;
    uint32_t x = 0, y = 0;
    for (const char *p = a; *p >= '0' && *p <= '9'; p++) x = x * 10 + (uint32_t)(*p - '0');
    for (const char *p = b; *p >= '0' && *p <= '9'; p++) y = y * 10 + (uint32_t)(*p - '0');
    if (x < 64 || y < 64 || x > 4096 || y > 4096) return 0;
    *w = x; *h = y;
    return 1;
}
