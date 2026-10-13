#ifndef P256_H
#define P256_H

#include <stdint.h>

/* NIST P-256 (secp256r1) and ECDSA signature verification.
 *
 * Most certificates issued today carry an ECDSA key on this curve rather than RSA, so
 * without this a large part of the web cannot be authenticated at all. Only verification
 * is implemented -- everything it touches is public (a signature, a public key, a hash),
 * so unlike a key exchange it has no secret whose timing could leak. */

#define P256_COORD_LEN 32

/* An uncompressed point as X.509 and TLS carry it: 0x04 || X(32) || Y(32). */
#define P256_PUBKEY_LEN 65

/* Verifies an ECDSA signature. `pub` is the 65-byte uncompressed point, `r` and `s` are
 * the signature's two 32-byte integers, and `digest` is the message hash (`digest_len`
 * bytes; longer digests are truncated to the curve's size, as FIPS 186-4 requires).
 * Returns 0 when the signature is valid, -1 otherwise. */
int p256_ecdsa_verify(const uint8_t pub[P256_PUBKEY_LEN],
                      const uint8_t r[32], const uint8_t s[32],
                      const uint8_t *digest, int digest_len);

#endif
