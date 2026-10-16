#include "imgload.h"
#include "uihost.h"
#include "png.h"
#include "jpeg.h"
#include "memory.h"
#include "string.h"

/* PNG or JPEG, recognised by their first bytes rather than the file name. */
int img_decode(const uint8_t *d, uint32_t len, uint8_t **rgba, uint32_t *w, uint32_t *h, const char **err)
{
    static char perr[80];
    if (len >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
        if (png_decode_rgba(d, len, rgba, w, h, perr, sizeof(perr)) != 0) { if (err) *err = perr; return -1; }
        return 0;
    }
    if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8) {
        uint8_t *rgb;
        const char *e = 0;
        if (jpeg_decode(d, len, &rgb, w, h, &e) != 0) { if (err) *err = e; return -1; }
        uint8_t *o = malloc((size_t)*w * *h * 4);
        if (!o) { free(rgb); if (err) *err = "out of memory"; return -1; }
        for (uint32_t i = 0; i < *w * *h; i++) { o[i * 4] = rgb[i * 3]; o[i * 4 + 1] = rgb[i * 3 + 1]; o[i * 4 + 2] = rgb[i * 3 + 2]; o[i * 4 + 3] = 255; }
        free(rgb);
        *rgba = o;
        return 0;
    }
    if (err) *err = "not a PNG or JPEG picture";
    return -1;
}

int img_load(const char *path, uint8_t **rgba, uint32_t *w, uint32_t *h, const char **err)
{
    uint8_t *d;
    uint32_t len;
    if (uihost_read(path, &d, &len) != 0) { if (err) *err = "could not read the file"; return -1; }
    int r = img_decode(d, len, rgba, w, h, err);
    free(d);
    return r;
}

/* Scales RGBA to a w x h RGBA copy, averaging over the source when shrinking (so text and fine
 * detail do not shimmer) and interpolating when enlarging. */
uint8_t *img_scale(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h)
{
    if (!w || !h) return 0;
    uint8_t *o = malloc((size_t)w * h * 4);
    if (!o) return 0;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t y0 = y * sh / h, y1 = (y + 1) * sh / h;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t x0 = x * sw / w, x1 = (x + 1) * sw / w;
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            uint32_t acc[4] = { 0, 0, 0, 0 }, n = 0;
            /* a box filter, sampling at most 4x4 source pixels per output pixel */
            uint32_t sy_step = (y1 - y0 + 3) / 4, sx_step = (x1 - x0 + 3) / 4;
            if (!sy_step) sy_step = 1;
            if (!sx_step) sx_step = 1;
            for (uint32_t sy = y0; sy < y1; sy += sy_step)
                for (uint32_t sx = x0; sx < x1; sx += sx_step) {
                    const uint8_t *p = src + ((size_t)sy * sw + sx) * 4;
                    acc[0] += p[0] * p[3]; acc[1] += p[1] * p[3]; acc[2] += p[2] * p[3]; acc[3] += p[3];
                    n++;
                }
            uint8_t *q = o + ((size_t)y * w + x) * 4;
            if (acc[3]) { q[0] = (uint8_t)(acc[0] / acc[3]); q[1] = (uint8_t)(acc[1] / acc[3]); q[2] = (uint8_t)(acc[2] / acc[3]); }
            else q[0] = q[1] = q[2] = 0;
            q[3] = (uint8_t)(acc[3] / n);
        }
    }
    return o;
}

/* ---- writing PNG ----
 * Stored (uncompressed) DEFLATE blocks: larger files than a real compressor makes, but every
 * PNG reader accepts them and it is a few dozen lines. */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const uint8_t *p, uint32_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    for (uint32_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static uint8_t *chunk(uint8_t *o, const char *type, const uint8_t *data, uint32_t len)
{
    put32(o, len);
    memcpy(o + 4, type, 4);
    if (len) memcpy(o + 8, data, len);
    uint32_t c = crc32_update(0xFFFFFFFFu, o + 4, len + 4) ^ 0xFFFFFFFFu;
    put32(o + 8 + len, c);
    return o + 12 + len;
}

int img_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    uint32_t row = w * 4 + 1, raw = row * h;
    uint32_t blocks = (raw + 65534) / 65535;
    uint32_t zlen = 2 + raw + blocks * 5 + 4;
    uint32_t total = 8 + 25 + 12 + zlen + 12;
    uint8_t *f = malloc(total), *z = malloc(zlen);
    if (!f || !z) { if (f) free(f); if (z) free(z); return -1; }
    /* zlib: header, stored blocks of the filtered rows (filter 0), Adler-32 */
    uint8_t *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0, done = 0;
    uint32_t r = 0, c = 0;                /* where in the rows the next byte comes from */
    while (done < raw) {
        uint32_t n = raw - done > 65535 ? 65535 : raw - done;
        *q++ = done + n == raw ? 1 : 0;
        *q++ = (uint8_t)n; *q++ = (uint8_t)(n >> 8); *q++ = (uint8_t)~n; *q++ = (uint8_t)(~n >> 8);
        for (uint32_t i = 0; i < n; i++) {
            uint8_t byte = c == 0 ? 0 : rgba[(size_t)r * w * 4 + (c - 1)];
            *q++ = byte;
            a = (a + byte) % 65521; b = (b + a) % 65521;
            if (++c == row) { c = 0; r++; }
        }
        done += n;
    }
    put32(q, (b << 16) | a);
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    memcpy(f, sig, 8);
    uint8_t ihdr[13];
    put32(ihdr, w); put32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    uint8_t *o = chunk(f + 8, "IHDR", ihdr, 13);
    o = chunk(o, "IDAT", z, zlen);
    o = chunk(o, "IEND", 0, 0);
    free(z);
    int rc = uihost_write(path, f, (uint32_t)(o - f));
    free(f);
    return rc;
}
