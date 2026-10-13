#ifndef TLS_H
#define TLS_H

#include <stdint.h>
#include "sha256.h"

/* TLS 1.2 client context */
#define TLS_RX_BUF  16384             /* TLS plaintext limit per record (RFC 5246 6.2.1) */
#define TLS_REC_MAX (16384 + 2048)    /* largest ciphertext record a peer may send */
#define TLS_TX_BUF  4096

/* The cipher suites this client offers, best first. */
#define TLS_SUITE_ECDHE_RSA_AES128_GCM_SHA256 0xC02F
#define TLS_SUITE_ECDHE_ECDSA_AES128_GCM_SHA256 0xC02B
#define TLS_SUITE_ECDHE_RSA_CHACHA20_SHA256     0xCCA8
#define TLS_SUITE_ECDHE_ECDSA_CHACHA20_SHA256   0xCCA9
#define TLS_SUITE_RSA_AES128_CBC_SHA256       0x003C

typedef struct {
    int      fd;
    uint16_t suite;                       /* TLS_SUITE_* the server chose */
    uint8_t  client_rand[32];
    uint8_t  server_rand[32];
    uint8_t  master[48];
    uint8_t  client_write_key[16];
    uint8_t  server_write_key[16];
    uint8_t  client_write_key32[32];      /* ChaCha20's keys are twice as long */
    uint8_t  server_write_key32[32];
    uint8_t  client_write_iv[16];         /* CBC: a whole block; GCM: the first 4 bytes are the fixed salt */
    uint8_t  server_write_iv[16];
    uint8_t  client_mac[32];
    uint8_t  server_mac[32];
    uint64_t tx_seq;
    uint64_t rx_seq;
    int      handshake_done;
    /* running SHA-256 over every handshake message, for the Finished messages
     * (a fixed 8 KiB transcript buffer used to cap the whole server flight,
     * certificate chain included) */
    sha256_t hs_hash;
    /* reassembly buffer for the server's handshake flight (heap, handshake only) */
    uint8_t *hs_in;
    uint32_t hs_in_len;
    uint32_t hs_in_pos;
    /* decrypted plaintext buffer */
    uint8_t  rx_plain[TLS_RX_BUF];
    uint32_t rx_plain_len;
    uint32_t rx_plain_pos;
} tls_ctx_t;

/* tls_connect()'s negative return codes -- numbered well past every
 * other layer's range (see tcp.h). A failed TCP connect propagates
 * tcp_connect()'s own code verbatim (an ARP_ERR_, IP_ERR_NOMEM, or
 * TCP_ERR_ value), so that case reports the real underlying reason
 * instead of a single generic "handshake or connection error" that
 * couldn't tell "the SYN never got a reply" apart from "the server
 * rejected our handshake". */
#define TLS_ERR_ALERT     -50 /* server sent a fatal alert (see dmesg for level/description) */
#define TLS_ERR_HANDSHAKE -51 /* handshake failed after TCP connected -- see dmesg for which step */
#define TLS_ERR_CERT      -52 /* server certificate rejected (tls_cert_error_detail() says why) */

/* Connect and perform TLS 1.2 handshake. sni_host, if non-NULL and
 * non-empty, is sent as the server_name extension -- required by
 * SNI-routing proxies/CDNs (e.g. Cloudflare) that terminate TLS for
 * many hostnames behind one IP and have no other way to tell which
 * one a given connection is for. Pass NULL to omit it. */
int  tls_connect(tls_ctx_t *ctx, uint32_t ip, uint16_t port, const char *sni_host);

/* Certificate checking is ON by default: the chain must lead to a trusted root
 * (x509.h: built-in store + x509_trust_add()), be valid now and cover the host
 * name passed to tls_connect(). tls_set_verify(0) turns that off (the server is
 * then not authenticated at all); callers must restore it themselves. */
void tls_set_verify(int on);
int  tls_get_verify(void);
/* Why the last TLS_ERR_CERT happened. */
const char *tls_cert_error_detail(void);
const char *tls_connect_strerror(int err);
/* Write application data */
int  tls_write(tls_ctx_t *ctx, const uint8_t *data, int len);
/* Read decrypted application data */
int  tls_read(tls_ctx_t *ctx, uint8_t *buf, int max);
/* Close TLS connection */
void tls_close(tls_ctx_t *ctx);

#endif
