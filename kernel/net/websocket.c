#include "websocket.h"
#include "tcp.h"
#include "tls.h"
#include "sha1.h"
#include "csprng.h"
#include "string.h"
#include "memory.h"
#include "klog.h"
#include "scheduler.h"

/* ---- the opening handshake ---- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *in, int len, char *out, int cap)
{
    int o = 0;
    for (int i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        if (o + 4 >= cap) return -1;
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* Either transport, so the framing code below does not care which one is in use. */
static int ws_write(ws_t *ws, const uint8_t *data, int len)
{
    if (ws->tls) return tls_write(ws->tls, data, len) == len ? 0 : -1;
    return tcp_send2(ws->fd, (void *)data, len);
}

static int ws_read_some(ws_t *ws, uint8_t *buf, int max)
{
    if (ws->tls) return tls_read(ws->tls, buf, max);
    return tcp_recv2(ws->fd, buf, max);
}

int ws_connect(ws_t *ws, uint32_t ip, const char *host, uint16_t port, const char *path, int use_tls)
{
    memset(ws, 0, sizeof(*ws));
    ws->fd = -1;

    if (use_tls) {
        ws->tls = (tls_ctx_t *)malloc(sizeof(tls_ctx_t));
        if (!ws->tls) return -1;
        int rc = tls_connect(ws->tls, ip, port, host);
        if (rc != 0) { free(ws->tls); ws->tls = 0; return rc; }
    } else {
        ws->fd = tcp_socket();
        if (ws->fd < 0) return -1;
        int rc = tcp_connect2(ws->fd, ip, port);
        if (rc != 0) { tcp_close2(ws->fd); ws->fd = -1; return rc; }
    }

    /* The key is 16 random bytes; the server must answer with the SHA-1 of it concatenated
     * with one fixed string, which is only there to prove the server understood the request
     * rather than being some other service that echoed the headers. */
    uint8_t keyraw[16];
    csprng_fill(keyraw, 16);
    char key[32];
    b64_encode(keyraw, 16, key, sizeof(key));

    char req[512];
    int n = 0;
    const char *parts[] = { "GET ", path, " HTTP/1.1\r\nHost: ", host,
                            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: ", key,
                            "\r\nSec-WebSocket-Version: 13\r\n\r\n", 0 };
    for (int i = 0; parts[i]; i++)
        for (const char *p = parts[i]; *p && n < (int)sizeof(req) - 1; p++) req[n++] = *p;
    req[n] = 0;
    if (ws_write(ws, (const uint8_t *)req, n) != 0) { ws_close(ws); return -1; }

    /* the response headers, read until the blank line */
    char resp[1024];
    int rn = 0, hdr_end = -1;
    for (int tries = 0; tries < 400 && hdr_end < 0; tries++) {
        int got = ws_read_some(ws, (uint8_t *)resp + rn, (int)sizeof(resp) - 1 - rn);
        if (got > 0) {
            rn += got;
            resp[rn] = 0;
            for (int i = 0; i + 3 < rn; i++)
                if (resp[i] == '\r' && resp[i+1] == '\n' && resp[i+2] == '\r' && resp[i+3] == '\n') { hdr_end = i + 4; break; }
        } else if (got < 0) break;
        else task_sleep(10);
        if (rn >= (int)sizeof(resp) - 1) break;
    }
    if (hdr_end < 0) { klog_write("ws: no handshake response\n"); ws_close(ws); return -1; }
    if (rn < 12 || strncmp(resp, "HTTP/1.1 101", 12) != 0) {
        klog_write("ws: server did not switch protocols\n");
        ws_close(ws);
        return -1;
    }

    /* check Sec-WebSocket-Accept */
    {
        char want[64];
        uint8_t dg[20];
        char cat[96];
        int cl = 0;
        for (const char *p = key; *p; p++) cat[cl++] = *p;
        const char *magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        for (const char *p = magic; *p; p++) cat[cl++] = *p;
        sha1_hash((const uint8_t *)cat, (uint32_t)cl, dg);
        b64_encode(dg, 20, want, sizeof(want));
        int ok = 0;
        for (int i = 0; i + (int)strlen(want) <= rn; i++)
            if (strncmp(resp + i, want, strlen(want)) == 0) { ok = 1; break; }
        if (!ok) { klog_write("ws: Sec-WebSocket-Accept does not match\n"); ws_close(ws); return -1; }
    }

    /* A server may start sending frames in the same packet as the handshake response, so
     * whatever came after the blank line is the beginning of the stream, not rubbish to
     * drop -- throwing it away loses the first message every time. */
    if (rn > hdr_end) {
        ws->rx_len = rn - hdr_end;
        memcpy(ws->rx, resp + hdr_end, (size_t)ws->rx_len);
    }

    ws->open = 1;
    klog_write("ws: connected\n");
    return 0;
}

/* ---- framing ---- */

int ws_send(ws_t *ws, int opcode, const uint8_t *data, int len)
{
    if (!ws->open || len < 0 || len > WS_MAX_FRAME) return -1;
    uint8_t hdr[14];
    int h = 0;
    hdr[h++] = (uint8_t)(0x80 | (opcode & 0x0F));           /* FIN set: one frame per message */
    uint8_t mask[4];
    csprng_fill(mask, 4);
    if (len < 126)        hdr[h++] = (uint8_t)(0x80 | len); /* the mask bit is mandatory for a client */
    else if (len < 65536) { hdr[h++] = 0x80 | 126; hdr[h++] = (uint8_t)(len >> 8); hdr[h++] = (uint8_t)len; }
    else return -1;
    memcpy(hdr + h, mask, 4); h += 4;
    if (ws_write(ws, hdr, h) != 0) return -1;
    if (len == 0) return 0;

    /* The payload is XORed with the mask; it buys nothing cryptographically and exists only
     * so that a client cannot be tricked into emitting bytes a proxy would read as HTTP. */
    uint8_t *masked = (uint8_t *)malloc((size_t)len);
    if (!masked) return -1;
    for (int i = 0; i < len; i++) masked[i] = data[i] ^ mask[i & 3];
    int rc = ws_write(ws, masked, len);
    free(masked);
    return rc;
}

int ws_send_text(ws_t *ws, const char *text)
{
    return ws_send(ws, WS_TEXT, (const uint8_t *)text, (int)strlen(text));
}

/* Pulls one complete frame out of the buffer; 0 when more bytes are needed. */
static int frame_ready(const uint8_t *b, int n, int *hdr_len, uint64_t *plen, int *opcode, int *fin)
{
    if (n < 2) return 0;
    *fin = (b[0] & 0x80) != 0;
    *opcode = b[0] & 0x0F;
    if (b[1] & 0x80) return -1;                 /* a server must never mask */
    uint64_t l = b[1] & 0x7F;
    int h = 2;
    if (l == 126) { if (n < 4) return 0; l = ((uint64_t)b[2] << 8) | b[3]; h = 4; }
    else if (l == 127) {
        if (n < 10) return 0;
        l = 0;
        for (int i = 0; i < 8; i++) l = (l << 8) | b[2 + i];
        h = 10;
    }
    if (l > WS_MAX_FRAME) return -1;
    if ((uint64_t)n < (uint64_t)h + l) return 0;
    *hdr_len = h;
    *plen = l;
    return 1;
}

int ws_recv(ws_t *ws, uint8_t *buf, int max, int *opcode, int timeout_ms)
{
    if (!ws->open) return -1;
    int waited = 0;
    for (;;) {
        int h, fin, op;
        uint64_t plen;
        int fr = frame_ready(ws->rx, ws->rx_len, &h, &plen, &op, &fin);
        if (fr < 0) { ws_close(ws); return -1; }
        if (fr == 1) {
            int n = (int)plen;
            if (op == WS_PING) {                             /* answer and keep waiting */
                ws_send(ws, WS_PONG, ws->rx + h, n);
            } else if (op == WS_CLOSE) {
                ws_send(ws, WS_CLOSE, 0, 0);
                ws->open = 0;
                memmove(ws->rx, ws->rx + h + n, (size_t)(ws->rx_len - h - n));
                ws->rx_len -= h + n;
                if (opcode) *opcode = WS_CLOSE;
                return -1;
            } else if (op != WS_PONG) {
                if (n > max) n = max;
                memcpy(buf, ws->rx + h, (size_t)n);
                if (opcode) *opcode = op;
                memmove(ws->rx, ws->rx + h + (int)plen, (size_t)(ws->rx_len - h - (int)plen));
                ws->rx_len -= h + (int)plen;
                return n;
            }
            memmove(ws->rx, ws->rx + h + (int)plen, (size_t)(ws->rx_len - h - (int)plen));
            ws->rx_len -= h + (int)plen;
            continue;
        }
        if (ws->rx_len >= WS_MAX_FRAME) { ws_close(ws); return -1; }
        int got = ws_read_some(ws, ws->rx + ws->rx_len, WS_MAX_FRAME - ws->rx_len);
        if (got > 0) { ws->rx_len += got; continue; }
        if (got < 0) { ws_close(ws); return -1; }
        if (waited >= timeout_ms) return 0;
        task_sleep(10);
        waited += 10;
    }
}

void ws_close(ws_t *ws)
{
    if (ws->open) ws_send(ws, WS_CLOSE, 0, 0);
    ws->open = 0;
    if (ws->tls) { tls_close(ws->tls); free(ws->tls); ws->tls = 0; }
    if (ws->fd >= 0) { tcp_close2(ws->fd); ws->fd = -1; }
}
