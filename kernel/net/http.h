#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

/* http_get() propagates whichever internal call failed -- tcp_connect()'s
 * TCP_ERR_* (or an ARP/IP code tcp_connect() itself propagated) --
 * verbatim (see tcp.h), rather than collapsing every failure into a
 * single generic value a caller can't tell apart.
 * `ip` must already be resolved by the caller -- `host` is only used
 * for the request's Host: header, never re-resolved here (a second,
 * independent DNS lookup right after the caller's own successful one
 * could itself fail and abort before tcp_connect() was ever reached,
 * misreporting a DNS hiccup as a connection error). */
#define HTTP_ERR_REQUEST -60   /* host/path too long or contains control characters */

#define HTTP_ERR_HEADER -61    /* response headers larger than HTTP_MAX_HEADER, or malformed chunking */
#define HTTP_MAX_HEADER 16384

/* Incremental framing of a response held in r[0..total): returns 1 once the
 * message is complete by Content-Length (*total is trimmed to it), 0 when more
 * data is needed, HTTP_ERR_HEADER when the header block is too large. */
int http_response_check(const uint8_t *r, int *total);
/* Decodes a 'Transfer-Encoding: chunked' body in place (headers kept).
 * Returns the new total length, or HTTP_ERR_HEADER if the chunking is invalid. */
int http_dechunk(uint8_t *r, int total);

int http_get(uint32_t ip, const char *host, uint16_t port, const char *path, uint8_t *response, int max_len);
const char *http_strerror(int err);

/* Builds "GET <path> HTTP/1.0" into req. Returns its length, or -1 when it
 * does not fit in cap or host/path contain control characters or spaces
 * (request splitting). */
int http_build_request(char *req, int cap, const char *host, const char *path);

/* The value of a response header (case-insensitive name, including the colon), or NULL.
 * The returned pointer is inside `resp` and runs to the end of the line. */
const char *http_header_value(const char *resp, int len, const char *name);

#endif
