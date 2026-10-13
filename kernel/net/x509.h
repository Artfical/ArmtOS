#ifndef X509_H
#define X509_H

#include <stdint.h>
#include "p256.h"

/* X.509 v3 certificate parsing, host name matching and chain validation
 * (RFC 5280 / RFC 6125 subset): RSA keys of 2048-4096 bits and PKCS#1 v1.5
 * signatures with SHA-256/384/512. SHA-1/MD5 signatures, RSASSA-PSS and ECDSA
 * are rejected, as are certificates carrying critical extensions this code does
 * not implement (name constraints, policy constraints, ...).
 * Revocation (CRL/OCSP) is NOT checked. */

#define X509_MAX_CHAIN 8
#define X509_MAX_CERT_LEN 16384

enum {
    X509_OK = 0,
    X509_ERR_MALFORMED = -1,        /* does not parse as a certificate */
    X509_ERR_UNSUPPORTED_KEY = -2,  /* subject key is not RSA 2048..4096 */
    X509_ERR_SIG_ALG = -3,          /* signature algorithm not supported */
    X509_ERR_WEAK_SIG_ALG = -4,     /* SHA-1 / MD5 based signature */
    X509_ERR_BAD_SIGNATURE = -5,
    X509_ERR_EXPIRED = -6,
    X509_ERR_NOT_YET_VALID = -7,
    X509_ERR_HOSTNAME = -8,
    X509_ERR_UNKNOWN_ISSUER = -9,   /* chain does not end at a trusted root */
    X509_ERR_NOT_CA = -10,          /* issuer is not a CA / may not sign certificates */
    X509_ERR_PATH_LEN = -11,
    X509_ERR_CRITICAL_EXT = -12,    /* unrecognised critical extension */
    X509_ERR_KEY_USAGE = -13,       /* extended/key usage does not allow this use */
    X509_ERR_CLOCK = -14,           /* system clock is not set, validity cannot be judged */
    X509_ERR_CHAIN_TOO_LONG = -15,
};

typedef struct {
    int32_t  day;    /* days since 1970-01-01 (negative before) */
    uint32_t sec;    /* seconds into that day */
} x509_time_t;

typedef struct {
    const uint8_t *der;     uint32_t der_len;
    const uint8_t *tbs;     uint32_t tbs_len;       /* the signed bytes, header included */
    const uint8_t *issuer;  uint32_t issuer_len;    /* Name, header included (compare bytewise) */
    const uint8_t *subject; uint32_t subject_len;
    const uint8_t *sig;     uint32_t sig_len;       /* signature value, without the unused-bits byte */
    int            sig_hash;                        /* RSA_HASH_*; 0 when sig_alg is not usable */
    int            sig_is_ecdsa;                    /* the signature is ECDSA (two INTEGERs), not PKCS#1 */
    int            sig_alg_status;                  /* X509_OK, X509_ERR_SIG_ALG or X509_ERR_WEAK_SIG_ALG */
    const uint8_t *n;       uint32_t n_len;         /* RSA modulus, leading zeros stripped */
    uint32_t       e;
    int            has_rsa_key;
    /* An EC key, the other kind of certificate in common use: `ec_curve` is an EC_CURVE_*
     * value and `ec_point` the uncompressed form (65 bytes on P-256, 97 on P-384). A curve
     * this code cannot do leaves has_ec_key clear. */
    int            has_ec_key, ec_curve;
    uint32_t       ec_point_len;
    uint8_t        ec_point[EC_MAX_PUBKEY_LEN];
    x509_time_t    not_before, not_after;
    int            has_bc, is_ca, path_len;         /* basicConstraints; path_len -1 = unlimited */
    int            has_ku;  uint16_t ku;            /* keyUsage: bit i set = named bit i of the BIT STRING (see X509_KU_*) */
    int            has_eku, eku_server_auth;        /* eku_server_auth also set for anyExtendedKeyUsage */
    int            has_san;
    const uint8_t *san;     uint32_t san_len;       /* SEQUENCE OF GeneralName contents */
    const uint8_t *cn;      uint32_t cn_len;        /* last subject commonName */
    int            critical_unsupported;
} x509_cert_t;

#define X509_KU_DIGITAL_SIGNATURE 0x0001
#define X509_KU_KEY_ENCIPHERMENT  0x0004
#define X509_KU_KEY_CERT_SIGN     0x0020

typedef struct { const uint8_t *der; uint32_t len; } x509_der_t;

/* Parses der into out (pointers refer into der, which must outlive out). */
int x509_parse(const uint8_t *der, uint32_t len, x509_cert_t *out);

/* Does the certificate cover host (a DNS name or a dotted-quad IPv4 literal)?
 * Returns 1 / 0. DNS names use dNSName SANs with RFC 6125 wildcards (one whole
 * left-most label, at least two labels behind it); the subject CN is only
 * consulted when the certificate has no SAN extension at all. */
int x509_host_matches(const x509_cert_t *c, const char *host);

/* Verifies c's signature with issuer's RSA key. */
int x509_check_signature(const x509_cert_t *c, const x509_cert_t *issuer);

/* Trusted roots: the built-in store plus any added at run time. */
int x509_trust_count(void);
x509_der_t x509_trust_get(int i);
int x509_trust_add(const uint8_t *der, uint32_t len);   /* copies; 0 or negative error */

#define X509_F_RSA_KEY_EXCHANGE 1   /* leaf must allow keyEncipherment (if it has keyUsage) */
#define X509_F_SIGNATURE        2   /* leaf must allow digitalSignature -- what an ECDHE handshake uses the key for */

/* Validates chain[0..n) (leaf first, the order servers send) against the trust
 * store for the server `host` at time `now`. On success returns 0 and parses
 * the leaf into *leaf. On failure returns an X509_ERR_* code. */
int x509_verify_chain(const x509_der_t *chain, int n, const char *host,
                      x509_time_t now, int flags, x509_cert_t *leaf);

const char *x509_strerror(int err);

/* Date helpers (UTC). */
x509_time_t x509_time_from_ymdhms(int y, int mo, int d, int h, int mi, int s);
int x509_time_cmp(x509_time_t a, x509_time_t b);

#endif
