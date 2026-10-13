#ifndef SHA1_H
#define SHA1_H

#include <stdint.h>

/* SHA-1 (FIPS 180-4). It is far too weak to sign anything with, and nothing here uses it
 * for that; it is present because the WebSocket handshake defines its one fixed check
 * value in terms of SHA-1 (RFC 6455), where the hash is not relied on for security. */
typedef struct {
    uint32_t h[5];
    uint8_t  buf[64];
    uint32_t buf_len;
    uint64_t total;
} sha1_t;

void sha1_init(sha1_t *s);
void sha1_update(sha1_t *s, const uint8_t *data, uint32_t len);
void sha1_final(sha1_t *s, uint8_t out[20]);
void sha1_hash(const uint8_t *data, uint32_t len, uint8_t out[20]);

#endif
