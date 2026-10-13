#ifndef P256_H
#define P256_H

#include <stdint.h>

/* NIST P-256 and P-384 (secp256r1 / secp384r1) with ECDSA signature verification.
 *
 * Most certificates issued today carry an EC key rather than RSA, and a real chain mixes
 * the two sizes -- a P-256 leaf is routinely signed by a P-384 root -- so both curves are
 * needed before an ordinary web site can be authenticated at all. Only verification is
 * implemented: everything it touches is public (a signature, a public key, a hash), so
 * unlike a key exchange it has no secret whose timing could leak.
 */

enum { EC_CURVE_P256 = 1, EC_CURVE_P384 = 2 };

/* Uncompressed point, 0x04 || X || Y: 65 bytes on P-256, 97 on P-384. */
#define EC_MAX_PUBKEY_LEN 97
#define EC_MAX_COORD_LEN  48

int ec_curve_coord_len(int curve);          /* 32 or 48; 0 for an unknown curve */

/* Verifies an ECDSA signature. `pub` is the uncompressed point, `r` and `s` are the
 * signature's two integers (each `coord_len` bytes, left-padded with zeros), and `digest`
 * is the message hash; a digest longer than the curve is truncated, as FIPS 186-4 requires.
 * Returns 0 when the signature is valid, -1 otherwise. */
int ec_ecdsa_verify(int curve, const uint8_t *pub, uint32_t pub_len,
                    const uint8_t *r, const uint8_t *s,
                    const uint8_t *digest, int digest_len);

#endif
