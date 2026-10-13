#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <stdint.h>
#include "tls.h"

/*
 * WebSocket client (RFC 6455), over plain TCP (ws://) or TLS (wss://).
 *
 * A WebSocket starts life as an HTTP request that asks to switch protocols; once the
 * server agrees, both sides send framed messages in either direction over the same
 * connection, which is what pages use for anything live -- chat, feeds, game state.
 */

#define WS_MAX_FRAME 65536

/* opcodes a caller sees */
enum { WS_TEXT = 1, WS_BINARY = 2, WS_CLOSE = 8, WS_PING = 9, WS_PONG = 10 };

typedef struct {
    int      fd;            /* plain TCP socket, or -1 when TLS is in use */
    tls_ctx_t *tls;         /* non-NULL for wss:// */
    int      open;
    uint8_t  rx[WS_MAX_FRAME];
    int      rx_len;        /* bytes buffered but not yet parsed */
} ws_t;

/* Connects and performs the opening handshake. `host` is used for the Host header and,
 * for wss://, for SNI and certificate checking. 0 = connected. */
int  ws_connect(ws_t *ws, uint32_t ip, const char *host, uint16_t port, const char *path, int use_tls);

/* Sends one message. Client frames are always masked, as RFC 6455 requires. */
int  ws_send(ws_t *ws, int opcode, const uint8_t *data, int len);
int  ws_send_text(ws_t *ws, const char *text);

/* Receives one message into buf. Returns its length and sets *opcode, 0 if nothing arrived
 * before the timeout, or negative on error/close. Ping frames are answered automatically. */
int  ws_recv(ws_t *ws, uint8_t *buf, int max, int *opcode, int timeout_ms);

void ws_close(ws_t *ws);

#endif
