#include "http.h"
#include "dns.h"
#include "tcp.h"
#include "net.h"

#include "arp.h"
#include "ip.h"
#include "string.h"
#include "memory.h"

static int req_put(char *req, int cap, int *off, const char *s, int check)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (check && (c <= 0x20 || c == 0x7F)) return -1;
        if (*off >= cap - 1) return -1;
        req[(*off)++] = (char)c;
    }
    return 0;
}

int http_build_request(char *req, int cap, const char *host, const char *path)
{
    int off = 0;
    if (req_put(req, cap, &off, "GET ", 0) || req_put(req, cap, &off, path, 1) ||
        req_put(req, cap, &off, " HTTP/1.0\r\nHost: ", 0) || req_put(req, cap, &off, host, 1) ||
        req_put(req, cap, &off, "\r\nConnection: close\r\n\r\n", 0))
        return -1;
    req[off] = 0;
    return off;
}

static int hdr_end(const uint8_t *r, int total)
{
    for (int i = 0; i + 3 < total; i++)
        if (r[i] == '\r' && r[i + 1] == '\n' && r[i + 2] == '\r' && r[i + 3] == '\n') return i + 4;
    return -1;
}

static int lc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Finds header `name` (lower case, with colon) inside the header block and
 * returns a pointer to its value, or 0. */
static const uint8_t *hdr_value(const uint8_t *r, int hlen, const char *name)
{
    int nl = (int)strlen(name);
    for (int i = 0; i + nl < hlen; i++) {
        if (i != 0 && r[i - 1] != '\n') continue;
        int k = 0;
        while (k < nl && lc(r[i + k]) == name[k]) k++;
        if (k == nl) {
            const uint8_t *v = r + i + nl;
            while (v < r + hlen && (*v == ' ' || *v == '\t')) v++;
            return v;
        }
    }
    return 0;
}

const char *http_header_value(const char *resp, int len, const char *name)
{
    const uint8_t *r = (const uint8_t *)resp;
    int he = hdr_end(r, len);
    if (he < 0) he = len;                       /* headers still incomplete: search what there is */
    return (const char *)hdr_value(r, he, name);
}

int http_response_check(const uint8_t *r, int *total)
{
    int he = hdr_end(r, *total);
    if (he < 0) return *total > HTTP_MAX_HEADER ? HTTP_ERR_HEADER : 0;
    if (he > HTTP_MAX_HEADER) return HTTP_ERR_HEADER;
    const uint8_t *v = hdr_value(r, he, "content-length:");
    if (!v) return 0;
    long cl = 0;
    int digits = 0;
    while (v < r + he && *v >= '0' && *v <= '9') {
        cl = cl * 10 + (*v - '0');
        if (++digits > 9) return HTTP_ERR_HEADER;   /* absurd length: no overflow */
        v++;
    }
    if (digits == 0) return 0;
    if ((long)(*total - he) >= cl) { *total = he + (int)cl; return 1; }
    return 0;
}

int http_dechunk(uint8_t *r, int total)
{
    int he = hdr_end(r, total);
    if (he < 0) return total;
    const uint8_t *te = hdr_value(r, he, "transfer-encoding:");
    if (!te || lc(te[0]) != 'c') return total;       /* not chunked */
    int in = he, out = he;
    for (;;) {
        long sz = 0;
        int digits = 0;
        while (in < total) {
            int c = lc(r[in]);
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (d < 0) break;
            sz = sz * 16 + d;
            if (++digits > 7) return HTTP_ERR_HEADER;
            in++;
        }
        if (digits == 0) return HTTP_ERR_HEADER;
        while (in < total && r[in] != '\n') in++;       /* skip chunk extensions */
        /* The caller's buffer may have filled up mid-body (a big page): what was received is
         * still a valid prefix, so a cut-off chunk returns that prefix instead of failing. */
        if (in >= total) return out;
        in++;
        if (sz == 0) return out;
        if (sz > total - in) {
            memmove(r + out, r + in, (size_t)(total - in));
            return out + (total - in);
        }
        memmove(r + out, r + in, (size_t)sz);
        out += (int)sz;
        in += (int)sz;
        if (in + 1 < total && r[in] == '\r' && r[in + 1] == '\n') in += 2;
        else if (in + 1 >= total) return out;              /* cut off before the chunk's CRLF */
        else return HTTP_ERR_HEADER;
    }
}

int http_get(uint32_t ip, const char *host, uint16_t port, const char *path, uint8_t *response, int max_len)
{
    int rc = tcp_connect(ip, port);
    if (rc != 0) return rc;

    char req[1024];
    int off = http_build_request(req, sizeof(req), host, path);
    if (off < 0) { tcp_close(); return HTTP_ERR_REQUEST; }

    if (tcp_send(req, off) != 0) { tcp_close(); return TCP_ERR_TIMEOUT; /* connection dropped between connect() and send() */ }

    int total = 0;
    while (total < max_len) {
        int n = tcp_recv(response + total, max_len - total);
        if (n <= 0) break;
        total += n;
        int st = http_response_check(response, &total);
        if (st < 0) { tcp_close(); return st; }
        if (st == 1) break;
    }

    tcp_close();
    return http_dechunk(response, total);
}

const char *http_strerror(int err)
{
    /* DNS_ERR_* occupies exactly -20..-24 (see dns.h); anything in that
     * band is dns_resolve()'s own code, everything else -- TCP_ERR_*,
     * or an ARP/IP code tcp_connect() propagated verbatim -- belongs to
     * tcp_connect_strerror(), which already falls back to
     * arp_resolve_strerror() for those. */
    if (err == HTTP_ERR_HEADER) return "malformed or oversized HTTP response headers";
    if (err == HTTP_ERR_REQUEST) return "request too long or invalid characters in URL";
    if (err <= -20 && err >= -25) return dns_strerror(err);
    return tcp_connect_strerror(err);
}
