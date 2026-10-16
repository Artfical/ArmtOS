#ifndef PNG_DECODE_H
#define PNG_DECODE_H

#include <stdint.h>

int png_decode(const uint8_t *file, uint32_t file_len, uint8_t **out_rgb,
                uint32_t *out_w, uint32_t *out_h, char *err, int err_cap);
/* the same, keeping transparency: 8-bit RGBA, any PNG bit depth, colour type or interlacing */
int png_decode_rgba(const uint8_t *file, uint32_t file_len, uint8_t **out_rgba,
                    uint32_t *out_w, uint32_t *out_h, char *err, int err_cap);

/* Decompresses a raw (no zlib wrapper) DEFLATE stream — the same
 * inflate engine png_decode() uses internally for IDAT data, exposed
 * standalone for anything else that needs to read DEFLATE-compressed
 * data, such as a ZIP archive's "deflated" (method 8) entries. Returns
 * 0 on success with *out_len set, -1 on a malformed/truncated stream
 * or if the output would exceed out_cap. */
int inflate_raw_buffer(const uint8_t *src, uint32_t src_len,
                        uint8_t *out, uint32_t out_cap, uint32_t *out_len);

/* Same as inflate_raw_buffer(), but also reports how many bytes of
 * `src` the DEFLATE stream actually consumed (in *consumed_len) --
 * needed when several streams are packed back-to-back with no length
 * prefix, such as git pack file entries. */
int inflate_raw_buffer_ex(const uint8_t *src, uint32_t src_len,
                           uint8_t *out, uint32_t out_cap, uint32_t *out_len,
                           uint32_t *consumed_len);

#endif
