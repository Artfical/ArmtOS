#include "jpeg.h"
#include "memory.h"
#include "string.h"

/*
 * A baseline JPEG (JFIF/EXIF) decoder: Huffman-coded 8-bit DCT, one (grey) or three (YCbCr)
 * components with any sampling factors up to 2x2, restart intervals. Progressive and
 * arithmetic-coded files are refused with a message. Integer arithmetic throughout, so the
 * ARM build -- which has no floating point in the kernel -- can use it as well.
 */

typedef struct { uint8_t bits[17]; uint8_t vals[256]; uint16_t code[256]; uint8_t len[256]; int count; } huff_t;

typedef struct {
    int id, h, v, tq;
    int td, ta;
    int pred;
    int bw, bh;                 /* blocks across and down, padded to whole MCUs */
    uint8_t *pix;               /* bw*8 x bh*8 samples */
} comp_t;

typedef struct {
    const uint8_t *p, *end;
    uint32_t buf; int bits;
    uint16_t qt[4][64];
    huff_t hdc[4], hac[4];
    comp_t c[3];
    int nc, w, h, hmax, vmax, restart;
    const char *err;
} jd_t;

static const uint8_t zz[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63 };

static int u16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

static void build_huff(huff_t *t)
{
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < t->bits[len]; i++) { t->code[k] = (uint16_t)code; t->len[k] = (uint8_t)len; code++; k++; }
        code <<= 1;
    }
    t->count = k;
}

/* the entropy-coded data: bytes with 0xFF00 stuffing, markers stop the stream */
static int get_bit(jd_t *d)
{
    if (!d->bits) {
        if (d->p >= d->end) { d->buf = 0; d->bits = 8; }
        else {
            uint8_t b = *d->p;
            if (b == 0xFF) {
                uint8_t n = d->p + 1 < d->end ? d->p[1] : 0;
                if (n == 0x00) { d->p += 2; }
                else { b = 0; }                      /* a marker: feed zeros, do not move past it */
            } else d->p++;
            d->buf = b; d->bits = 8;
        }
    }
    d->bits--;
    return (int)((d->buf >> d->bits) & 1);
}

static int get_bits(jd_t *d, int n) { int v = 0; while (n--) v = (v << 1) | get_bit(d); return v; }

static int decode_huff(jd_t *d, huff_t *t)
{
    int code = 0;
    int k = 0;
    for (int len = 1; len <= 16; len++) {
        code = (code << 1) | get_bit(d);
        for (int i = 0; i < t->bits[len]; i++, k++)
            if (t->code[k] == code) return t->vals[k];
    }
    d->err = "corrupt Huffman data";
    return 0;
}

static int extend(int v, int n) { return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }

/* inverse DCT in fixed point: the textbook separable form with a 13-bit cosine table */
static const int idct_c[8][8] = {
#define C(a) (a)
    { 2896, 4017, 3784, 3406, 2896, 2276, 1567, 799 },
    { 2896, 3406, 1567, -799, -2896, -4017, -3784, -2276 },
    { 2896, 2276, -1567, -4017, -2896, 799, 3784, 3406 },
    { 2896, 799, -3784, -2276, 2896, 3406, -1567, -4017 },
    { 2896, -799, -3784, 2276, 2896, -3406, -1567, 4017 },
    { 2896, -2276, -1567, 4017, -2896, -799, 3784, -3406 },
    { 2896, -3406, 1567, 799, -2896, 4017, -3784, 2276 },
    { 2896, -4017, 3784, -3406, 2896, -2276, 1567, -799 },
#undef C
};
/* idct_c[x][u] = C(u) * cos((2x+1) u pi / 16) * 4096 * sqrt(2)/2 for u = 0 */

static void idct(const int *in, uint8_t *out, int stride)
{
    int tmp[64];
    for (int y = 0; y < 8; y++)                    /* rows */
        for (int x = 0; x < 8; x++) {
            int s = 0;
            for (int u = 0; u < 8; u++) s += in[y * 8 + u] * idct_c[x][u];
            tmp[y * 8 + x] = s >> 11;              /* keep 1 extra bit */
        }
    for (int x = 0; x < 8; x++)                    /* columns */
        for (int y = 0; y < 8; y++) {
            int s = 0;
            for (int v = 0; v < 8; v++) s += tmp[v * 8 + x] * idct_c[y][v];
            int val = ((s + 16384) >> 15) + 128;   /* 2 * 4096 from the passes, and the 1/4 of the 2-D IDCT */
            out[y * stride + x] = (uint8_t)(val < 0 ? 0 : val > 255 ? 255 : val);
        }
}

static void decode_block(jd_t *d, comp_t *c, uint8_t *out, int stride)
{
    int coef[64];
    memset(coef, 0, sizeof(coef));
    int t = decode_huff(d, &d->hdc[c->td]);
    int diff = t ? extend(get_bits(d, t), t) : 0;
    c->pred += diff;
    coef[0] = c->pred * d->qt[c->tq][0];
    for (int k = 1; k < 64;) {
        int rs = decode_huff(d, &d->hac[c->ta]);
        int r = rs >> 4, s = rs & 15;
        if (!s) { if (r != 15) break; k += 16; continue; }
        k += r;
        if (k > 63) break;
        coef[zz[k]] = extend(get_bits(d, s), s) * d->qt[c->tq][k];
        k++;
    }
    idct(coef, out, stride);
}

static int parse(jd_t *d)
{
    while (d->p + 4 <= d->end) {
        if (d->p[0] != 0xFF) { d->p++; continue; }
        uint8_t m = d->p[1];
        d->p += 2;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0xFF) { if (m == 0xFF) d->p--; continue; }
        if (m == 0xD9) break;
        int len = u16(d->p);
        const uint8_t *seg = d->p + 2, *next = d->p + len;
        if (next > d->end) { d->err = "truncated file"; return -1; }
        if (m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE) { d->err = "progressive JPEG (only baseline is supported)"; return -1; }
        if (m >= 0xC9 && m <= 0xCF && m != 0xCC) { d->err = "arithmetic-coded JPEG is not supported"; return -1; }
        if (m == 0xDB) {                                     /* quantisation tables */
            const uint8_t *q = seg;
            while (q < next) {
                int pq = q[0] >> 4, tq = q[0] & 3;
                q++;
                for (int i = 0; i < 64; i++) { d->qt[tq][i] = pq ? (uint16_t)u16(q + i * 2) : q[i]; }
                q += pq ? 128 : 64;
            }
        } else if (m == 0xC4) {                              /* Huffman tables */
            const uint8_t *q = seg;
            while (q < next) {
                int tc = q[0] >> 4, th = q[0] & 3;
                huff_t *t = tc ? &d->hac[th] : &d->hdc[th];
                int total = 0;
                for (int i = 1; i <= 16; i++) { t->bits[i] = q[i]; total += q[i]; }
                if (total > 256) { d->err = "bad Huffman table"; return -1; }
                memcpy(t->vals, q + 17, (size_t)total);
                build_huff(t);
                q += 17 + total;
            }
        } else if (m == 0xC0 || m == 0xC1) {                 /* baseline frame */
            if (seg[0] != 8) { d->err = "only 8-bit JPEG is supported"; return -1; }
            d->h = u16(seg + 1);
            d->w = u16(seg + 3);
            d->nc = seg[5];
            if (d->nc != 1 && d->nc != 3) { d->err = "only grey or colour (YCbCr) JPEG is supported"; return -1; }
            for (int i = 0; i < d->nc; i++) {
                d->c[i].id = seg[6 + i * 3];
                d->c[i].h = seg[7 + i * 3] >> 4;
                d->c[i].v = seg[7 + i * 3] & 15;
                d->c[i].tq = seg[8 + i * 3] & 3;
                if (d->c[i].h > d->hmax) d->hmax = d->c[i].h;
                if (d->c[i].v > d->vmax) d->vmax = d->c[i].v;
            }
        } else if (m == 0xDD) {
            d->restart = u16(seg);
        } else if (m == 0xDA) {                              /* start of scan: the image data follows */
            int ns = seg[0];
            for (int i = 0; i < ns; i++) {
                int id = seg[1 + i * 2];
                for (int k = 0; k < d->nc; k++)
                    if (d->c[k].id == id) { d->c[k].td = seg[2 + i * 2] >> 4; d->c[k].ta = seg[2 + i * 2] & 15; }
            }
            d->p = next;
            return 0;
        }
        d->p = next;
    }
    d->err = "no image data";
    return -1;
}

int jpeg_decode(const uint8_t *file, uint32_t len, uint8_t **out_rgb, uint32_t *out_w, uint32_t *out_h, const char **err)
{
    jd_t *d = malloc(sizeof(jd_t));
    if (!d) { if (err) *err = "out of memory"; return -1; }
    memset(d, 0, sizeof(*d));
    d->p = file; d->end = file + len;
    if (len < 4 || file[0] != 0xFF || file[1] != 0xD8) { free(d); if (err) *err = "not a JPEG"; return -1; }
    if (parse(d) != 0 || !d->w || !d->h || !d->hmax) {
        if (err) *err = d->err ? d->err : "unreadable JPEG";
        free(d);
        return -1;
    }
    if ((uint32_t)d->w * d->h > 16u * 1024 * 1024) { free(d); if (err) *err = "picture too large"; return -1; }
    int mcu_w = 8 * d->hmax, mcu_h = 8 * d->vmax;
    int mx = (d->w + mcu_w - 1) / mcu_w, my = (d->h + mcu_h - 1) / mcu_h;
    int ok = 1;
    for (int i = 0; i < d->nc; i++) {
        comp_t *c = &d->c[i];
        c->bw = mx * c->h; c->bh = my * c->v;
        c->pix = malloc((size_t)c->bw * 8 * c->bh * 8);
        if (!c->pix) ok = 0;
    }
    uint8_t *rgb = ok ? malloc((size_t)d->w * d->h * 3) : 0;
    if (!rgb) {
        for (int i = 0; i < d->nc; i++) if (d->c[i].pix) free(d->c[i].pix);
        free(d);
        if (err) *err = "out of memory";
        return -1;
    }

    int mcus = 0;
    for (int by = 0; by < my && !d->err; by++)
        for (int bx = 0; bx < mx && !d->err; bx++) {
            if (d->restart && mcus && mcus % d->restart == 0) {
                /* byte-align and step over the RSTn marker */
                d->bits = 0;
                while (d->p + 1 < d->end && !(d->p[0] == 0xFF && d->p[1] >= 0xD0 && d->p[1] <= 0xD7)) d->p++;
                if (d->p + 1 < d->end) d->p += 2;
                for (int i = 0; i < d->nc; i++) d->c[i].pred = 0;
            }
            for (int i = 0; i < d->nc; i++) {
                comp_t *c = &d->c[i];
                int stride = c->bw * 8;
                for (int v = 0; v < c->v; v++)
                    for (int h = 0; h < c->h; h++) {
                        int px = (bx * c->h + h) * 8, py = (by * c->v + v) * 8;
                        decode_block(d, c, c->pix + py * stride + px, stride);
                    }
            }
            mcus++;
        }

    /* upsample and convert to RGB (JFIF YCbCr, fixed point) */
    for (int y = 0; y < d->h; y++)
        for (int x = 0; x < d->w; x++) {
            uint8_t *o = rgb + ((size_t)y * d->w + x) * 3;
            comp_t *c0 = &d->c[0];
            int Y = c0->pix[(y * c0->v / d->vmax) * c0->bw * 8 + x * c0->h / d->hmax];
            if (d->nc == 1) { o[0] = o[1] = o[2] = (uint8_t)Y; continue; }
            comp_t *c1 = &d->c[1], *c2 = &d->c[2];
            int cb = c1->pix[(y * c1->v / d->vmax) * c1->bw * 8 + x * c1->h / d->hmax] - 128;
            int cr = c2->pix[(y * c2->v / d->vmax) * c2->bw * 8 + x * c2->h / d->hmax] - 128;
            int r = Y + ((91881 * cr) >> 16);
            int g = Y - ((22554 * cb + 46802 * cr) >> 16);
            int b = Y + ((116130 * cb) >> 16);
            o[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
            o[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
            o[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
        }
    for (int i = 0; i < d->nc; i++) free(d->c[i].pix);
    const char *e = d->err;
    uint32_t w = (uint32_t)d->w, h = (uint32_t)d->h;
    free(d);
    if (e && err) *err = e;          /* a damaged tail still gives the part that decoded */
    *out_rgb = rgb; *out_w = w; *out_h = h;
    return 0;
}
