#include <stdint.h>
#include <stddef.h>
#include "png.h"

#ifdef PNG_HOST_TEST
#include <stdlib.h>
#include <string.h>
#define PNG_MALLOC malloc
#define PNG_FREE free
#define PNG_MEMCMP memcmp
#else
#include "memory.h"
#include "string.h"
#define PNG_MALLOC malloc
#define PNG_FREE free
#define PNG_MEMCMP memcmp
#endif

#define MAXBITS 15
/* filtered scanlines of up to 16 million pixels: the old 800 KB cap refused anything bigger
 * than about 500x500 in colour, which ruled out any screen-sized picture */
#define MAX_RAW_SIZE (64u * 1024u * 1024u)

typedef struct {
    short count[MAXBITS + 1];
    short symbol[288];
} huff_t;

typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint32_t pos;
    uint32_t bitbuf;
    int bitcnt;
} bitreader_t;

static int br_getbit(bitreader_t *br)
{
    if (br->bitcnt == 0) {
        if (br->pos >= br->len) return -1;
        br->bitbuf = br->data[br->pos++];
        br->bitcnt = 8;
    }
    int bit = br->bitbuf & 1;
    br->bitbuf >>= 1;
    br->bitcnt--;
    return bit;
}

static int32_t br_getbits(bitreader_t *br, int n)
{
    uint32_t val = 0;
    for (int i = 0; i < n; i++) {
        int b = br_getbit(br);
        if (b < 0) return -1;
        val |= ((uint32_t)b) << i;
    }
    return (int32_t)val;
}

static void br_align(bitreader_t *br)
{
    br->bitbuf = 0;
    br->bitcnt = 0;
}

static void build_huffman(huff_t *h, const uint8_t *lengths, int n)
{
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[lengths[sym]]++;
    int offs[MAXBITS + 2];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS + 1; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++) {
        if (lengths[sym]) h->symbol[offs[lengths[sym]]++] = (short)sym;
    }
}

static int decode_symbol(bitreader_t *br, huff_t *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int bit = br_getbit(br);
        if (bit < 0) return -1;
        code |= bit;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const uint8_t clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int inflate_block_huffman(bitreader_t *br, uint8_t *out, uint32_t out_cap, uint32_t *op_io,
                                  huff_t *lit, huff_t *dist)
{
    uint32_t op = *op_io;
    for (;;) {
        int sym = decode_symbol(br, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (op >= out_cap) return -1;
            out[op++] = (uint8_t)sym;
        } else if (sym == 256) {
            break;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int32_t extra = length_extra[sym] ? br_getbits(br, length_extra[sym]) : 0;
            if (extra < 0) return -1;
            uint32_t length = length_base[sym] + (uint32_t)extra;

            int dsym = decode_symbol(br, dist);
            if (dsym < 0 || dsym >= 30) return -1;
            int32_t dextra = dist_extra[dsym] ? br_getbits(br, dist_extra[dsym]) : 0;
            if (dextra < 0) return -1;
            uint32_t distance = dist_base[dsym] + (uint32_t)dextra;

            if (distance > op) return -1;
            if (op + length > out_cap) return -1;
            uint32_t src = op - distance;
            for (uint32_t i = 0; i < length; i++) out[op++] = out[src++];
        }
    }
    *op_io = op;
    return 0;
}

static int inflate_raw(bitreader_t *br, uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    uint32_t op = 0;
    huff_t fixed_lit, fixed_dist;
    int fixed_built = 0;

    for (;;) {
        int32_t bfinal = br_getbits(br, 1);
        int32_t btype = br_getbits(br, 2);
        if (bfinal < 0 || btype < 0) return -1;

        if (btype == 0) {
            br_align(br);
            if (br->pos + 4 > br->len) return -1;
            uint32_t lenv = br->data[br->pos] | ((uint32_t)br->data[br->pos + 1] << 8);
            uint32_t nlen = br->data[br->pos + 2] | ((uint32_t)br->data[br->pos + 3] << 8);
            br->pos += 4;
            if ((lenv ^ 0xFFFFu) != nlen) return -1;
            if (br->pos + lenv > br->len) return -1;
            if (op + lenv > out_cap) return -1;
            memcpy(out + op, br->data + br->pos, lenv);
            op += lenv;
            br->pos += lenv;
        } else if (btype == 1) {
            if (!fixed_built) {
                uint8_t lit_lengths[288];
                for (int i = 0; i < 144; i++) lit_lengths[i] = 8;
                for (int i = 144; i < 256; i++) lit_lengths[i] = 9;
                for (int i = 256; i < 280; i++) lit_lengths[i] = 7;
                for (int i = 280; i < 288; i++) lit_lengths[i] = 8;
                build_huffman(&fixed_lit, lit_lengths, 288);
                uint8_t dist_lengths[30];
                for (int i = 0; i < 30; i++) dist_lengths[i] = 5;
                build_huffman(&fixed_dist, dist_lengths, 30);
                fixed_built = 1;
            }
            if (inflate_block_huffman(br, out, out_cap, &op, &fixed_lit, &fixed_dist) != 0) return -1;
        } else if (btype == 2) {
            int32_t hlit_r = br_getbits(br, 5);
            int32_t hdist_r = br_getbits(br, 5);
            int32_t hclen_r = br_getbits(br, 4);
            if (hlit_r < 0 || hdist_r < 0 || hclen_r < 0) return -1;
            int hlit = hlit_r + 257;
            int hdist = hdist_r + 1;
            int hclen = hclen_r + 4;

            uint8_t clen_lengths[19];
            for (int i = 0; i < 19; i++) clen_lengths[i] = 0;
            for (int i = 0; i < hclen; i++) {
                int32_t v = br_getbits(br, 3);
                if (v < 0) return -1;
                clen_lengths[clen_order[i]] = (uint8_t)v;
            }
            huff_t clen_huff;
            build_huffman(&clen_huff, clen_lengths, 19);

            int total = hlit + hdist;
            uint8_t lengths[288 + 32];
            int idx = 0;
            while (idx < total) {
                int sym = decode_symbol(br, &clen_huff);
                if (sym < 0) return -1;
                if (sym < 16) {
                    lengths[idx++] = (uint8_t)sym;
                } else if (sym == 16) {
                    if (idx == 0) return -1;
                    int32_t rep = br_getbits(br, 2);
                    if (rep < 0) return -1;
                    rep += 3;
                    uint8_t prev = lengths[idx - 1];
                    while (rep-- > 0 && idx < total) lengths[idx++] = prev;
                } else if (sym == 17) {
                    int32_t rep = br_getbits(br, 3);
                    if (rep < 0) return -1;
                    rep += 3;
                    while (rep-- > 0 && idx < total) lengths[idx++] = 0;
                } else {
                    int32_t rep = br_getbits(br, 7);
                    if (rep < 0) return -1;
                    rep += 11;
                    while (rep-- > 0 && idx < total) lengths[idx++] = 0;
                }
            }

            huff_t lit, dist;
            build_huffman(&lit, lengths, hlit);
            build_huffman(&dist, lengths + hlit, hdist);
            if (inflate_block_huffman(br, out, out_cap, &op, &lit, &dist) != 0) return -1;
        } else {
            return -1;
        }

        if (bfinal) break;
    }

    *out_len = op;
    return 0;
}

int inflate_raw_buffer(const uint8_t *src, uint32_t src_len,
                        uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    bitreader_t br = { src, src_len, 0, 0, 0 };
    return inflate_raw(&br, out, out_cap, out_len);
}

int inflate_raw_buffer_ex(const uint8_t *src, uint32_t src_len,
                           uint8_t *out, uint32_t out_cap, uint32_t *out_len,
                           uint32_t *consumed_len)
{
    bitreader_t br = { src, src_len, 0, 0, 0 };
    int rc = inflate_raw(&br, out, out_cap, out_len);
    if (rc == 0) *consumed_len = br.pos;
    return rc;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void seterr(char *err, int cap, const char *msg)
{
    if (!err || cap <= 0) return;
    int i = 0;
    while (msg[i] && i < cap - 1) { err[i] = msg[i]; i++; }
    err[i] = 0;
}

/*
 * Every PNG the format allows: bit depths 1, 2, 4, 8 and 16, grey, RGB, palette, grey+alpha
 * and RGBA, Adam7 interlacing, and tRNS transparency. The result is 8-bit RGBA.
 */
int png_decode_rgba(const uint8_t *file, uint32_t file_len, uint8_t **out_rgba,
                    uint32_t *out_w, uint32_t *out_h, char *err, int err_cap)
{
    static const uint8_t png_sig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    if (file_len < 8 || PNG_MEMCMP(file, png_sig, 8) != 0) { seterr(err, err_cap, "not a PNG file"); return -1; }

    uint32_t pos = 8, width = 0, height = 0;
    uint8_t bitdepth = 0, colortype = 0, interlace = 0;
    static uint8_t palette[256][4];
    int pal_count = 0, have_ihdr = 0;
    int trns_gray = -1, trns_r = -1, trns_g = -1, trns_b = -1;
    uint8_t *idat = NULL;
    uint32_t idat_len = 0, idat_cap = 0;
    for (int i = 0; i < 256; i++) palette[i][3] = 255;

    while (pos + 8 <= file_len) {
        uint32_t clen = be32(file + pos);
        const uint8_t *ctype = file + pos + 4, *cdata = file + pos + 8;
        if ((uint64_t)pos + 8 + (uint64_t)clen + 4 > file_len) {
            seterr(err, err_cap, "truncated PNG chunk");
            if (idat) PNG_FREE(idat);
            return -1;
        }
        if (PNG_MEMCMP(ctype, "IHDR", 4) == 0) {
            if (clen < 13) { seterr(err, err_cap, "bad IHDR"); if (idat) PNG_FREE(idat); return -1; }
            width = be32(cdata); height = be32(cdata + 4);
            bitdepth = cdata[8]; colortype = cdata[9]; interlace = cdata[12];
            if (cdata[10] != 0 || interlace > 1) {
                seterr(err, err_cap, "unknown PNG compression or interlace method");
                if (idat) PNG_FREE(idat);
                return -1;
            }
            have_ihdr = 1;
        } else if (PNG_MEMCMP(ctype, "PLTE", 4) == 0) {
            pal_count = (int)(clen / 3);
            if (pal_count > 256) pal_count = 256;
            for (int i = 0; i < pal_count; i++) { palette[i][0] = cdata[i * 3]; palette[i][1] = cdata[i * 3 + 1]; palette[i][2] = cdata[i * 3 + 2]; }
        } else if (PNG_MEMCMP(ctype, "tRNS", 4) == 0) {
            if (colortype == 3) { for (uint32_t i = 0; i < clen && i < 256; i++) palette[i][3] = cdata[i]; }
            else if (colortype == 0 && clen >= 2) trns_gray = (int)((cdata[0] << 8) | cdata[1]);
            else if (colortype == 2 && clen >= 6) {
                trns_r = (cdata[0] << 8) | cdata[1]; trns_g = (cdata[2] << 8) | cdata[3]; trns_b = (cdata[4] << 8) | cdata[5];
            }
        } else if (PNG_MEMCMP(ctype, "IDAT", 4) == 0) {
            if (idat_len + clen > idat_cap) {
                uint32_t newcap = idat_cap ? idat_cap * 2 : 4096;
                while (newcap < idat_len + clen) newcap *= 2;
                uint8_t *nb = (uint8_t *)PNG_MALLOC(newcap);
                if (!nb) { seterr(err, err_cap, "out of memory"); if (idat) PNG_FREE(idat); return -1; }
                if (idat) { memcpy(nb, idat, idat_len); PNG_FREE(idat); }
                idat = nb; idat_cap = newcap;
            }
            memcpy(idat + idat_len, cdata, clen);
            idat_len += clen;
        } else if (PNG_MEMCMP(ctype, "IEND", 4) == 0) break;
        pos += 8 + clen + 4;
    }
    if (!have_ihdr || !idat || width == 0 || height == 0) {
        seterr(err, err_cap, "missing IHDR/IDAT");
        if (idat) PNG_FREE(idat);
        return -1;
    }

    int channels;
    switch (colortype) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: seterr(err, err_cap, "unsupported PNG color type"); PNG_FREE(idat); return -1;
    }
    int depth_ok = (bitdepth == 8 || bitdepth == 16) ||
                   ((colortype == 0 || colortype == 3) && (bitdepth == 1 || bitdepth == 2 || bitdepth == 4));
    if (!depth_ok || (colortype == 3 && bitdepth == 16)) { seterr(err, err_cap, "invalid PNG bit depth"); PNG_FREE(idat); return -1; }
    if (colortype == 3 && pal_count == 0) { seterr(err, err_cap, "palette PNG missing PLTE"); PNG_FREE(idat); return -1; }

    /* the seven Adam7 passes, or one pass covering everything */
    static const int ax0[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay0[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const int adx[7] = { 8, 8, 4, 4, 2, 2, 1 }, ady[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int npass = interlace ? 7 : 1;
    uint64_t bits_pp = (uint64_t)channels * bitdepth;
    uint32_t fbpp = (uint32_t)((bits_pp + 7) / 8);            /* bytes per pixel, for the filters */
    uint64_t raw_need = 0;
    uint32_t pw[7], ph[7], prow[7];
    for (int p = 0; p < npass; p++) {
        uint32_t x0 = interlace ? (uint32_t)ax0[p] : 0, y0 = interlace ? (uint32_t)ay0[p] : 0;
        uint32_t dx = interlace ? (uint32_t)adx[p] : 1, dy = interlace ? (uint32_t)ady[p] : 1;
        pw[p] = width > x0 ? (width - x0 + dx - 1) / dx : 0;
        ph[p] = height > y0 ? (height - y0 + dy - 1) / dy : 0;
        uint64_t rb = (pw[p] * bits_pp + 7) / 8;
        if (rb > 0xFFFFFFF) { seterr(err, err_cap, "image too large"); PNG_FREE(idat); return -1; }
        prow[p] = (uint32_t)rb;
        if (pw[p] && ph[p]) raw_need += (rb + 1) * ph[p];
    }
    if (raw_need == 0 || raw_need > MAX_RAW_SIZE || (uint64_t)width * height > MAX_RAW_SIZE / 4) {
        seterr(err, err_cap, "image too large");
        PNG_FREE(idat);
        return -1;
    }
    if (idat_len < 2 || (idat[0] & 0x0F) != 8 || (idat[1] & 0x20)) {
        seterr(err, err_cap, "unsupported zlib stream");
        PNG_FREE(idat);
        return -1;
    }
    uint8_t *raw = (uint8_t *)PNG_MALLOC(raw_need);
    if (!raw) { seterr(err, err_cap, "out of memory"); PNG_FREE(idat); return -1; }
    bitreader_t br;
    br.data = idat; br.len = idat_len; br.pos = 2; br.bitbuf = 0; br.bitcnt = 0;
    uint32_t raw_len = 0;
    int rc = inflate_raw(&br, raw, (uint32_t)raw_need, &raw_len);
    PNG_FREE(idat);
    if (rc != 0 || raw_len != raw_need) { seterr(err, err_cap, "DEFLATE decode failed (corrupt PNG?)"); PNG_FREE(raw); return -1; }

    uint8_t *rgba = (uint8_t *)PNG_MALLOC((uint64_t)width * height * 4);
    if (!rgba) { seterr(err, err_cap, "out of memory"); PNG_FREE(raw); return -1; }

    uint8_t *rp = raw;
    uint32_t maxval = (1u << (bitdepth < 8 ? bitdepth : 8)) - 1;
    for (int p = 0; p < npass; p++) {
        if (!pw[p] || !ph[p]) continue;
        uint32_t stride = prow[p];
        uint8_t *prev = NULL;
        for (uint32_t y = 0; y < ph[p]; y++) {
            uint8_t ftype = rp[0];
            uint8_t *row = rp + 1;
            for (uint32_t x = 0; x < stride; x++) {
                int a = x >= fbpp ? row[x - fbpp] : 0;
                int b = prev ? prev[x] : 0;
                int c = (prev && x >= fbpp) ? prev[x - fbpp] : 0;
                switch (ftype) {
                    case 0: break;
                    case 1: row[x] = (uint8_t)(row[x] + a); break;
                    case 2: row[x] = (uint8_t)(row[x] + b); break;
                    case 3: row[x] = (uint8_t)(row[x] + ((a + b) >> 1)); break;
                    case 4: {
                        int q = a + b - c;
                        int pa = q > a ? q - a : a - q, pb = q > b ? q - b : b - q, pc = q > c ? q - c : c - q;
                        row[x] = (uint8_t)(row[x] + ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c)));
                        break;
                    }
                    default:
                        seterr(err, err_cap, "unsupported PNG filter type");
                        PNG_FREE(rgba); PNG_FREE(raw);
                        return -1;
                }
            }
            uint32_t oy = interlace ? (uint32_t)ay0[p] + y * (uint32_t)ady[p] : y;
            for (uint32_t x = 0; x < pw[p]; x++) {
                uint32_t ox = interlace ? (uint32_t)ax0[p] + x * (uint32_t)adx[p] : x;
                uint8_t *o = rgba + ((uint64_t)oy * width + ox) * 4;
                uint32_t v[4] = { 0, 0, 0, 0 }, v16[4] = { 0, 0, 0, 0 };
                for (int ch = 0; ch < channels; ch++) {
                    if (bitdepth == 16) {
                        const uint8_t *q = row + ((uint64_t)x * channels + ch) * 2;
                        v16[ch] = (uint32_t)((q[0] << 8) | q[1]);
                        v[ch] = q[0];
                    } else if (bitdepth == 8) {
                        v[ch] = v16[ch] = row[(uint64_t)x * channels + ch];
                    } else {
                        uint64_t bit = (uint64_t)x * bitdepth;
                        uint32_t byte = row[bit / 8];
                        v16[ch] = (byte >> (8 - bitdepth - (bit % 8))) & maxval;
                        v[ch] = colortype == 3 ? v16[ch] : v16[ch] * 255 / maxval;
                    }
                }
                switch (colortype) {
                    case 0: o[0] = o[1] = o[2] = (uint8_t)v[0]; o[3] = (trns_gray >= 0 && (int)v16[0] == trns_gray) ? 0 : 255; break;
                    case 2:
                        o[0] = (uint8_t)v[0]; o[1] = (uint8_t)v[1]; o[2] = (uint8_t)v[2];
                        o[3] = (trns_r >= 0 && (int)v16[0] == trns_r && (int)v16[1] == trns_g && (int)v16[2] == trns_b) ? 0 : 255;
                        break;
                    case 3: {
                        uint32_t idx = v[0] < (uint32_t)pal_count ? v[0] : 0;
                        o[0] = palette[idx][0]; o[1] = palette[idx][1]; o[2] = palette[idx][2]; o[3] = palette[idx][3];
                        break;
                    }
                    case 4: o[0] = o[1] = o[2] = (uint8_t)v[0]; o[3] = (uint8_t)v[1]; break;
                    default: o[0] = (uint8_t)v[0]; o[1] = (uint8_t)v[1]; o[2] = (uint8_t)v[2]; o[3] = (uint8_t)v[3]; break;
                }
            }
            prev = row;
            rp += stride + 1;
        }
    }
    PNG_FREE(raw);
    *out_rgba = rgba;
    *out_w = width;
    *out_h = height;
    return 0;
}

/* The older RGB interface: transparent parts are laid over white. */
int png_decode(const uint8_t *file, uint32_t file_len, uint8_t **out_rgb,
               uint32_t *out_w, uint32_t *out_h, char *err, int err_cap)
{
    uint8_t *rgba;
    uint32_t w, h;
    if (png_decode_rgba(file, file_len, &rgba, &w, &h, err, err_cap) != 0) return -1;
    uint8_t *rgb = (uint8_t *)PNG_MALLOC((uint64_t)w * h * 3);
    if (!rgb) { PNG_FREE(rgba); seterr(err, err_cap, "out of memory"); return -1; }
    for (uint64_t i = 0; i < (uint64_t)w * h; i++) {
        uint32_t a = rgba[i * 4 + 3];
        for (int c = 0; c < 3; c++) rgb[i * 3 + c] = (uint8_t)((rgba[i * 4 + c] * a + 255 * (255 - a)) / 255);
    }
    PNG_FREE(rgba);
    *out_rgb = rgb; *out_w = w; *out_h = h;
    return 0;
}
