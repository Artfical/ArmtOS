#include "x509.h"
#include "rsa.h"
#include "p256.h"
#include "sha256.h"
#include "sha512.h"
#include "string.h"
#include "memory.h"

/* ------------------------------------------------------------------ DER */

typedef struct { const uint8_t *p, *end; } der_it;

/* Reads one TLV. Strict DER: single-byte tags, definite lengths in minimal
 * form (at most 3 length bytes, i.e. < 16 MiB), contents inside the parent. */
static int der_next(der_it *it, uint8_t *tag, const uint8_t **val, uint32_t *vlen, const uint8_t **start)
{
    const uint8_t *p = it->p;
    if (start) *start = p;
    if ((uint32_t)(it->end - p) < 2) return -1;
    uint8_t t = p[0];
    if ((t & 0x1F) == 0x1F) return -1;
    uint8_t b = p[1];
    const uint8_t *q = p + 2;
    uint32_t len;
    if (b < 0x80) {
        len = b;
    } else {
        int nb = b & 0x7F;
        if (nb == 0 || nb > 3) return -1;
        if ((uint32_t)(it->end - q) < (uint32_t)nb) return -1;
        if (q[0] == 0) return -1;
        len = 0;
        for (int i = 0; i < nb; i++) len = (len << 8) | q[i];
        q += nb;
        if (len < 0x80) return -1;
    }
    if ((uint32_t)(it->end - q) < len) return -1;
    *tag = t;
    *val = q;
    *vlen = len;
    it->p = q + len;
    return 0;
}

static int der_expect(der_it *it, uint8_t want, const uint8_t **val, uint32_t *vlen, const uint8_t **start)
{
    uint8_t tag;
    if (der_next(it, &tag, val, vlen, start) != 0 || tag != want) return -1;
    return 0;
}

static int der_done(const der_it *it) { return it->p == it->end; }

static der_it der_sub(const uint8_t *val, uint32_t len)
{
    der_it it = { val, val + len };
    return it;
}

/* -------------------------------------------------------------- OIDs --- */

static int oid_is(const uint8_t *v, uint32_t l, const uint8_t *oid, uint32_t ol)
{
    return l == ol && memcmp(v, oid, ol) == 0;
}
#define OID_IS(v, l, ...) oid_is((v), (l), (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }))

/* ------------------------------------------------------------- dates --- */

static int days_in_month(int y, int m)
{
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
}

x509_time_t x509_time_from_ymdhms(int y, int mo, int d, int h, int mi, int s)
{
    int yy = y - (mo <= 2);
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    int yoe = yy - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    x509_time_t t;
    t.day = era * 146097 + doe - 719468;
    t.sec = (uint32_t)(h * 3600 + mi * 60 + s);
    return t;
}

int x509_time_cmp(x509_time_t a, x509_time_t b)
{
    if (a.day != b.day) return a.day < b.day ? -1 : 1;
    if (a.sec != b.sec) return a.sec < b.sec ? -1 : 1;
    return 0;
}

static int two(const uint8_t *p)
{
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

/* UTCTime (YYMMDDHHMMSSZ) or GeneralizedTime (YYYYMMDDHHMMSSZ), UTC only. */
static int parse_time(uint8_t tag, const uint8_t *v, uint32_t l, x509_time_t *out)
{
    int y, off;
    if (tag == 0x17) {
        if (l != 13) return -1;
        int yy = two(v);
        if (yy < 0) return -1;
        y = yy >= 50 ? 1900 + yy : 2000 + yy;
        off = 2;
    } else if (tag == 0x18) {
        if (l != 15) return -1;
        int hi = two(v), lo = two(v + 2);
        if (hi < 0 || lo < 0) return -1;
        y = hi * 100 + lo;
        off = 4;
    } else {
        return -1;
    }
    int mo = two(v + off), d = two(v + off + 2), h = two(v + off + 4);
    int mi = two(v + off + 6), s = two(v + off + 8);
    if (v[off + 10] != 'Z') return -1;
    if (mo < 1 || mo > 12 || d < 1 || h < 0 || mi < 0 || s < 0) return -1;
    if (d > days_in_month(y, mo) || h > 23 || mi > 59 || s > 60) return -1;
    if (s == 60) s = 59;
    *out = x509_time_from_ymdhms(y, mo, d, h, mi, s);
    return 0;
}

/* --------------------------------------------------- field extraction --- */

static int parse_sig_alg(const uint8_t *alg_val, uint32_t alg_len, int *hash, int *status, int *is_ecdsa)
{
    der_it it = der_sub(alg_val, alg_len);
    const uint8_t *oid; uint32_t ol;
    *is_ecdsa = 0;
    if (der_expect(&it, 0x06, &oid, &ol, 0) != 0) return -1;
    /* parameters: absent or NULL for the PKCS#1 v1.5 family; an ECDSA algorithm omits them */
    if (!der_done(&it)) {
        const uint8_t *pv; uint32_t pl;
        if (der_expect(&it, 0x05, &pv, &pl, 0) != 0 || pl != 0 || !der_done(&it)) {
            *hash = 0; *status = X509_ERR_SIG_ALG;
            return 0;
        }
    }
    *hash = 0;
    if (OID_IS(oid, ol, 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0B)) { *hash = RSA_HASH_SHA256; *status = X509_OK; }
    else if (OID_IS(oid, ol, 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0C)) { *hash = RSA_HASH_SHA384; *status = X509_OK; }
    else if (OID_IS(oid, ol, 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x0D)) { *hash = RSA_HASH_SHA512; *status = X509_OK; }
    else if (OID_IS(oid, ol, 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x05) ||   /* sha1WithRSA */
             OID_IS(oid, ol, 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x04) ||   /* md5WithRSA */
             OID_IS(oid, ol, 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x02))     /* md2WithRSA */
        *status = X509_ERR_WEAK_SIG_ALG;
    else if (OID_IS(oid, ol, 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02)) { *hash = RSA_HASH_SHA256; *status = X509_OK; *is_ecdsa = 1; }
    else if (OID_IS(oid, ol, 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x03)) { *hash = RSA_HASH_SHA384; *status = X509_OK; *is_ecdsa = 1; }
    else if (OID_IS(oid, ol, 0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x04)) { *hash = RSA_HASH_SHA512; *status = X509_OK; *is_ecdsa = 1; }
    else
        *status = X509_ERR_SIG_ALG;                       /* PSS, ... */
    return 0;
}

/* SubjectPublicKeyInfo -> RSA modulus/exponent when it is an RSA key. */
static int parse_spki(const uint8_t *v, uint32_t l, x509_cert_t *c)
{
    der_it it = der_sub(v, l);
    const uint8_t *av; uint32_t al;
    if (der_expect(&it, 0x30, &av, &al, 0) != 0) return -1;
    const uint8_t *kv; uint32_t kl;
    if (der_expect(&it, 0x03, &kv, &kl, 0) != 0 || !der_done(&it) || kl < 1) return -1;

    der_it ai = der_sub(av, al);
    const uint8_t *oid; uint32_t ol;
    if (der_expect(&ai, 0x06, &oid, &ol, 0) != 0) return -1;
    c->has_rsa_key = 0;
    c->has_ec_key = 0;
    /* id-ecPublicKey (1.2.840.10045.2.1) with the prime256v1 parameters: the key is the
     * uncompressed point in the BIT STRING. Any other curve is left unusable rather than
     * guessed at. */
    if (OID_IS(oid, ol, 0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01)) {
        const uint8_t *cv; uint32_t cl;
        if (der_expect(&ai, 0x06, &cv, &cl, 0) != 0 || !der_done(&ai)) return 0;
        int curve = 0;
        if (OID_IS(cv, cl, 0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07)) curve = EC_CURVE_P256;   /* prime256v1 */
        else if (OID_IS(cv, cl, 0x2B,0x81,0x04,0x00,0x22)) curve = EC_CURVE_P384;             /* secp384r1 */
        else return 0;                                                                        /* a curve we cannot do */
        uint32_t want = 1 + 2 * (uint32_t)ec_curve_coord_len(curve);
        if (kv[0] != 0 || kl != 1 + want || kv[1] != 4) return 0;    /* unused bits / not uncompressed */
        memcpy(c->ec_point, kv + 1, want);
        c->ec_point_len = want;
        c->ec_curve = curve;
        c->has_ec_key = 1;
        return 0;
    }
    if (!OID_IS(oid, ol, 0x2A,0x86,0x48,0x86,0xF7,0x0D,0x01,0x01,0x01)) return 0;   /* not RSA: fine until used */
    if (kv[0] != 0) return -1;                                                        /* unused bits */

    der_it ki = der_sub(kv + 1, kl - 1);
    const uint8_t *sv; uint32_t sl;
    if (der_expect(&ki, 0x30, &sv, &sl, 0) != 0 || !der_done(&ki)) return -1;
    der_it si = der_sub(sv, sl);
    const uint8_t *nv; uint32_t nl;
    const uint8_t *ev; uint32_t el;
    if (der_expect(&si, 0x02, &nv, &nl, 0) != 0 || der_expect(&si, 0x02, &ev, &el, 0) != 0 || !der_done(&si)) return -1;
    if (nl == 0 || el == 0 || (nv[0] & 0x80) || (ev[0] & 0x80)) return -1;           /* negative */
    while (nl > 1 && nv[0] == 0) { nv++; nl--; }
    while (el > 1 && ev[0] == 0) { ev++; el--; }
    if (el > 4) return 0;                                                            /* huge exponent: unusable */
    if (nl < RSA_MIN_BYTES || nl > RSA_MAX_BYTES || (nl & 3)) return 0;              /* unusable size */
    uint32_t e = 0;
    for (uint32_t i = 0; i < el; i++) e = (e << 8) | ev[i];
    c->n = nv; c->n_len = nl; c->e = e; c->has_rsa_key = 1;
    return 0;
}

static void parse_cn(const uint8_t *name, uint32_t len, x509_cert_t *c)
{
    der_it rdns = der_sub(name, len);
    for (;;) {
        if (der_done(&rdns)) return;
        const uint8_t *sv; uint32_t sl;
        if (der_expect(&rdns, 0x31, &sv, &sl, 0) != 0) return;
        der_it set = der_sub(sv, sl);
        while (!der_done(&set)) {
            const uint8_t *av; uint32_t al;
            if (der_expect(&set, 0x30, &av, &al, 0) != 0) return;
            der_it at = der_sub(av, al);
            const uint8_t *oid; uint32_t ol;
            uint8_t vt; const uint8_t *vv; uint32_t vl;
            if (der_expect(&at, 0x06, &oid, &ol, 0) != 0) return;
            if (der_next(&at, &vt, &vv, &vl, 0) != 0) return;
            if (OID_IS(oid, ol, 0x55,0x04,0x03) && (vt == 0x0C || vt == 0x13 || vt == 0x16 || vt == 0x14)) {
                c->cn = vv; c->cn_len = vl;
            }
        }
    }
}

static int parse_extension(const uint8_t *oid, uint32_t ol, int critical, const uint8_t *v, uint32_t l, x509_cert_t *c)
{
    der_it it;
    if (OID_IS(oid, ol, 0x55,0x1D,0x0F)) {                        /* keyUsage */
        const uint8_t *bv; uint32_t bl;
        it = der_sub(v, l);
        if (der_expect(&it, 0x03, &bv, &bl, 0) != 0 || !der_done(&it) || bl < 2) return -1;
        uint16_t ku = 0;
        for (int bit = 0; bit < 16; bit++) {
            uint32_t byte = 1 + (uint32_t)bit / 8;
            if (byte < bl && (bv[byte] & (0x80 >> (bit % 8)))) ku |= (uint16_t)(1u << bit);
        }
        c->has_ku = 1; c->ku = ku;
    } else if (OID_IS(oid, ol, 0x55,0x1D,0x13)) {                 /* basicConstraints */
        const uint8_t *sv; uint32_t sl;
        it = der_sub(v, l);
        if (der_expect(&it, 0x30, &sv, &sl, 0) != 0 || !der_done(&it)) return -1;
        der_it bc = der_sub(sv, sl);
        c->has_bc = 1; c->is_ca = 0; c->path_len = -1;
        if (!der_done(&bc)) {
            uint8_t tag; const uint8_t *tv; uint32_t tl;
            if (der_next(&bc, &tag, &tv, &tl, 0) != 0) return -1;
            if (tag == 0x01) {
                if (tl != 1) return -1;
                c->is_ca = tv[0] != 0;
                if (!der_done(&bc)) {
                    if (der_next(&bc, &tag, &tv, &tl, 0) != 0) return -1;
                }
            }
            if (tag == 0x02) {
                if (tl < 1 || tl > 2 || (tv[0] & 0x80)) return -1;
                int pl = tv[0];
                if (tl == 2) pl = (pl << 8) | tv[1];
                c->path_len = pl;
            }
        }
    } else if (OID_IS(oid, ol, 0x55,0x1D,0x11)) {                 /* subjectAltName */
        const uint8_t *sv; uint32_t sl;
        it = der_sub(v, l);
        if (der_expect(&it, 0x30, &sv, &sl, 0) != 0 || !der_done(&it)) return -1;
        der_it gn = der_sub(sv, sl);
        while (!der_done(&gn)) {
            uint8_t tag; const uint8_t *gv; uint32_t gl;
            if (der_next(&gn, &tag, &gv, &gl, 0) != 0) return -1;
        }
        c->has_san = 1; c->san = sv; c->san_len = sl;
    } else if (OID_IS(oid, ol, 0x55,0x1D,0x25)) {                 /* extendedKeyUsage */
        const uint8_t *sv; uint32_t sl;
        it = der_sub(v, l);
        if (der_expect(&it, 0x30, &sv, &sl, 0) != 0 || !der_done(&it)) return -1;
        der_it ek = der_sub(sv, sl);
        c->has_eku = 1;
        while (!der_done(&ek)) {
            const uint8_t *ov; uint32_t olen;
            if (der_expect(&ek, 0x06, &ov, &olen, 0) != 0) return -1;
            if (OID_IS(ov, olen, 0x2B,0x06,0x01,0x05,0x05,0x07,0x03,0x01) ||      /* serverAuth */
                OID_IS(ov, olen, 0x55,0x1D,0x25,0x00))                             /* anyExtendedKeyUsage */
                c->eku_server_auth = 1;
        }
    } else if (critical &&
               !OID_IS(oid, ol, 0x55,0x1D,0x20) &&       /* certificatePolicies: accepted, not evaluated */
               !OID_IS(oid, ol, 0x55,0x1D,0x0E) &&       /* subjectKeyIdentifier */
               !OID_IS(oid, ol, 0x55,0x1D,0x23)) {       /* authorityKeyIdentifier */
        c->critical_unsupported = 1;                      /* nameConstraints, policyConstraints, ... */
    }
    return 0;
}

int x509_parse(const uint8_t *der, uint32_t len, x509_cert_t *c)
{
    memset(c, 0, sizeof(*c));
    c->path_len = -1;
    if (!der || len < 16 || len > X509_MAX_CERT_LEN) return X509_ERR_MALFORMED;
    c->der = der; c->der_len = len;

    der_it top = { der, der + len };
    const uint8_t *cv; uint32_t cl;
    if (der_expect(&top, 0x30, &cv, &cl, 0) != 0 || !der_done(&top)) return X509_ERR_MALFORMED;

    der_it cert = der_sub(cv, cl);
    const uint8_t *tv, *tstart; uint32_t tl;
    if (der_expect(&cert, 0x30, &tv, &tl, &tstart) != 0) return X509_ERR_MALFORMED;
    c->tbs = tstart; c->tbs_len = (uint32_t)(cert.p - tstart);

    const uint8_t *av, *astart; uint32_t al;
    if (der_expect(&cert, 0x30, &av, &al, &astart) != 0) return X509_ERR_MALFORMED;
    const uint8_t *outer_alg = astart; uint32_t outer_alg_len = (uint32_t)(cert.p - astart);
    if (parse_sig_alg(av, al, &c->sig_hash, &c->sig_alg_status, &c->sig_is_ecdsa) != 0) return X509_ERR_MALFORMED;

    const uint8_t *sgv; uint32_t sgl;
    if (der_expect(&cert, 0x03, &sgv, &sgl, 0) != 0 || !der_done(&cert) || sgl < 2 || sgv[0] != 0) return X509_ERR_MALFORMED;
    c->sig = sgv + 1; c->sig_len = sgl - 1;

    /* TBSCertificate */
    der_it t = der_sub(tv, tl);
    uint8_t tag; const uint8_t *v; uint32_t l;
    const uint8_t *st;
    int version = 0;
    if (der_next(&t, &tag, &v, &l, &st) != 0) return X509_ERR_MALFORMED;
    if (tag == 0xA0) {                                            /* [0] version */
        der_it vi = der_sub(v, l);
        const uint8_t *iv; uint32_t il;
        if (der_expect(&vi, 0x02, &iv, &il, 0) != 0 || !der_done(&vi) || il != 1 || iv[0] > 2) return X509_ERR_MALFORMED;
        version = iv[0];
        if (der_next(&t, &tag, &v, &l, &st) != 0) return X509_ERR_MALFORMED;
    }
    if (tag != 0x02 || l == 0 || l > 21) return X509_ERR_MALFORMED;   /* serialNumber */

    if (der_expect(&t, 0x30, &v, &l, &st) != 0) return X509_ERR_MALFORMED;           /* signature */
    if ((uint32_t)(t.p - st) != outer_alg_len || memcmp(st, outer_alg, outer_alg_len) != 0) return X509_ERR_MALFORMED;

    if (der_expect(&t, 0x30, &v, &l, &st) != 0) return X509_ERR_MALFORMED;           /* issuer */
    c->issuer = st; c->issuer_len = (uint32_t)(t.p - st);

    if (der_expect(&t, 0x30, &v, &l, 0) != 0) return X509_ERR_MALFORMED;            /* validity */
    {
        der_it vi = der_sub(v, l);
        uint8_t t1, t2; const uint8_t *v1, *v2; uint32_t l1, l2;
        if (der_next(&vi, &t1, &v1, &l1, 0) != 0 || der_next(&vi, &t2, &v2, &l2, 0) != 0 || !der_done(&vi)) return X509_ERR_MALFORMED;
        if (parse_time(t1, v1, l1, &c->not_before) != 0 || parse_time(t2, v2, l2, &c->not_after) != 0) return X509_ERR_MALFORMED;
    }

    if (der_expect(&t, 0x30, &v, &l, &st) != 0) return X509_ERR_MALFORMED;           /* subject */
    c->subject = st; c->subject_len = (uint32_t)(t.p - st);
    parse_cn(v, l, c);

    if (der_expect(&t, 0x30, &v, &l, 0) != 0) return X509_ERR_MALFORMED;            /* subjectPublicKeyInfo */
    if (parse_spki(v, l, c) != 0) return X509_ERR_MALFORMED;

    while (!der_done(&t)) {
        if (der_next(&t, &tag, &v, &l, 0) != 0) return X509_ERR_MALFORMED;
        if (tag == 0xA1 || tag == 0xA2) continue;                                     /* unique IDs */
        if (tag != 0xA3 || version != 2) return X509_ERR_MALFORMED;                   /* extensions need v3 */
        der_it ex = der_sub(v, l);
        const uint8_t *sv; uint32_t sl;
        if (der_expect(&ex, 0x30, &sv, &sl, 0) != 0 || !der_done(&ex)) return X509_ERR_MALFORMED;
        der_it exts = der_sub(sv, sl);
        while (!der_done(&exts)) {
            const uint8_t *ev; uint32_t el;
            if (der_expect(&exts, 0x30, &ev, &el, 0) != 0) return X509_ERR_MALFORMED;
            der_it e1 = der_sub(ev, el);
            const uint8_t *oid; uint32_t ol;
            if (der_expect(&e1, 0x06, &oid, &ol, 0) != 0) return X509_ERR_MALFORMED;
            int critical = 0;
            uint8_t et; const uint8_t *xv; uint32_t xl;
            if (der_next(&e1, &et, &xv, &xl, 0) != 0) return X509_ERR_MALFORMED;
            if (et == 0x01) {
                if (xl != 1) return X509_ERR_MALFORMED;
                critical = xv[0] != 0;
                if (der_next(&e1, &et, &xv, &xl, 0) != 0) return X509_ERR_MALFORMED;
            }
            if (et != 0x04 || !der_done(&e1)) return X509_ERR_MALFORMED;
            if (parse_extension(oid, ol, critical, xv, xl, c) != 0) return X509_ERR_MALFORMED;
        }
    }
    return X509_OK;
}

/* ------------------------------------------------------ host matching --- */

static int lower(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }

static int host_is_ipv4(const char *h, uint8_t out[4])
{
    int part = 0, val = 0, digits = 0;
    for (const char *p = h;; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            if (++digits > 3 || val > 255) return 0;
        } else if (*p == '.' || *p == 0) {
            if (digits == 0 || part > 3) return 0;
            out[part++] = (uint8_t)val;
            val = 0; digits = 0;
            if (*p == 0) break;
        } else {
            return 0;
        }
    }
    return part == 4;
}

static int dns_name_ok(const uint8_t *p, uint32_t l)
{
    if (l == 0) return 0;
    for (uint32_t i = 0; i < l; i++) if (p[i] <= 0x20 || p[i] >= 0x7F) return 0;   /* also rejects embedded NULs */
    return 1;
}

static int dns_match(const uint8_t *pat, uint32_t pl, const char *host)
{
    uint32_t hl = (uint32_t)strlen(host);
    if (hl && host[hl - 1] == '.') hl--;
    if (pl && pat[pl - 1] == '.') pl--;
    if (!dns_name_ok(pat, pl) || hl == 0) return 0;

    if (pl >= 2 && pat[0] == '*') {
        if (pat[1] != '.') return 0;                          /* only a whole left-most label */
        const uint8_t *rest = pat + 2; uint32_t rl = pl - 2;
        for (uint32_t i = 0; i < rl; i++) if (rest[i] == '*') return 0;
        int dot = 0;
        for (uint32_t i = 0; i < rl; i++) if (rest[i] == '.') dot = 1;
        if (!dot) return 0;                                   /* "*.com" would cover a whole TLD */
        uint32_t i = 0;
        while (i < hl && host[i] != '.') i++;                 /* host's first label */
        if (i == 0 || i >= hl) return 0;                      /* needs a non-empty label and a dot */
        const char *hrest = host + i + 1; uint32_t hrl = hl - i - 1;
        if (hrl != rl) return 0;
        for (uint32_t k = 0; k < rl; k++) if (lower(hrest[k]) != lower(rest[k])) return 0;
        return 1;
    }
    for (uint32_t i = 0; i < pl; i++) if (pat[i] == '*') return 0;
    if (pl != hl) return 0;
    for (uint32_t i = 0; i < pl; i++) if (lower(pat[i]) != lower(host[i])) return 0;
    return 1;
}

int x509_host_matches(const x509_cert_t *c, const char *host)
{
    if (!host || !*host) return 0;
    uint8_t ip[4];
    int is_ip = host_is_ipv4(host, ip);

    if (c->has_san) {
        der_it gn = der_sub(c->san, c->san_len);
        while (!der_done(&gn)) {
            uint8_t tag; const uint8_t *gv; uint32_t gl;
            if (der_next(&gn, &tag, &gv, &gl, 0) != 0) return 0;
            if (is_ip) {
                if (tag == 0x87 && gl == 4 && memcmp(gv, ip, 4) == 0) return 1;
            } else if (tag == 0x82) {
                if (dns_match(gv, gl, host)) return 1;
            }
        }
        return 0;                                             /* SAN present: the CN is not consulted */
    }
    if (is_ip || !c->cn) return 0;
    return dns_match(c->cn, c->cn_len, host);
}

/* ---------------------------------------------------------- signature --- */

/* An ECDSA signature is DER: SEQUENCE { INTEGER r, INTEGER s }, each of which may carry a
 * leading zero (or be short) and has to be laid out as a fixed-width value for the curve. */
static int ecdsa_sig_split(const uint8_t *sig, uint32_t len, uint8_t *r, uint8_t *s, int clen)
{
    der_it it = der_sub(sig, len);
    const uint8_t *sv; uint32_t sl;
    if (der_expect(&it, 0x30, &sv, &sl, 0) != 0 || !der_done(&it)) return -1;
    der_it si = der_sub(sv, sl);
    const uint8_t *rv, *svv; uint32_t rl, svl;
    if (der_expect(&si, 0x02, &rv, &rl, 0) != 0 || der_expect(&si, 0x02, &svv, &svl, 0) != 0 || !der_done(&si)) return -1;
    if (rl == 0 || svl == 0 || (rv[0] & 0x80) || (svv[0] & 0x80)) return -1;   /* negative */
    while (rl > 1 && rv[0] == 0) { rv++; rl--; }
    while (svl > 1 && svv[0] == 0) { svv++; svl--; }
    if ((int)rl > clen || (int)svl > clen) return -1;
    memset(r, 0, (size_t)clen); memcpy(r + (clen - (int)rl), rv, rl);
    memset(s, 0, (size_t)clen); memcpy(s + (clen - (int)svl), svv, svl);
    return 0;
}

int x509_check_signature(const x509_cert_t *c, const x509_cert_t *issuer)
{
    if (c->sig_alg_status != X509_OK) return c->sig_alg_status;
    uint8_t digest[64];
    int dlen;
    switch (c->sig_hash) {
    case RSA_HASH_SHA256: sha256_hash(c->tbs, c->tbs_len, digest); dlen = 32; break;
    case RSA_HASH_SHA384: sha384_hash(c->tbs, c->tbs_len, digest); dlen = 48; break;
    case RSA_HASH_SHA512: sha512_hash(c->tbs, c->tbs_len, digest); dlen = 64; break;
    default: return X509_ERR_SIG_ALG;
    }
    if (c->sig_is_ecdsa) {
        /* The signature algorithm names the key it must have been made with: an ECDSA
         * signature can only be checked against the issuer's EC key. */
        if (!issuer->has_ec_key) return X509_ERR_UNSUPPORTED_KEY;
        int clen = ec_curve_coord_len(issuer->ec_curve);
        uint8_t r[EC_MAX_COORD_LEN], sg[EC_MAX_COORD_LEN];
        if (clen == 0 || ecdsa_sig_split(c->sig, c->sig_len, r, sg, clen) != 0) return X509_ERR_BAD_SIGNATURE;
        return ec_ecdsa_verify(issuer->ec_curve, issuer->ec_point, issuer->ec_point_len,
                               r, sg, digest, dlen) == 0
               ? X509_OK : X509_ERR_BAD_SIGNATURE;
    }
    if (!issuer->has_rsa_key) return X509_ERR_UNSUPPORTED_KEY;
    if (c->sig_len != issuer->n_len) return X509_ERR_BAD_SIGNATURE;
    return rsa_pkcs1_verify(issuer->n, issuer->n_len, issuer->e, c->sig_hash, digest, c->sig, c->sig_len) == 0
           ? X509_OK : X509_ERR_BAD_SIGNATURE;
}

/* -------------------------------------------------------- trust store --- */

extern const x509_der_t ca_store_builtin[];
extern const int ca_store_builtin_count;

#define MAX_USER_ROOTS 16
static x509_der_t user_roots[MAX_USER_ROOTS];
static int user_root_count;

int x509_trust_count(void) { return ca_store_builtin_count + user_root_count; }

x509_der_t x509_trust_get(int i)
{
    x509_der_t none = { 0, 0 };
    if (i < 0) return none;
    if (i < ca_store_builtin_count) return ca_store_builtin[i];
    i -= ca_store_builtin_count;
    return i < user_root_count ? user_roots[i] : none;
}

int x509_trust_add(const uint8_t *der, uint32_t len)
{
    x509_cert_t c;
    int rc = x509_parse(der, len, &c);
    if (rc != X509_OK) return rc;
    if (!c.has_rsa_key) return X509_ERR_UNSUPPORTED_KEY;
    for (int i = 0; i < x509_trust_count(); i++) {
        x509_der_t r = x509_trust_get(i);
        if (r.len == len && memcmp(r.der, der, len) == 0) return X509_OK;      /* already trusted */
    }
    if (user_root_count >= MAX_USER_ROOTS) return X509_ERR_CHAIN_TOO_LONG;
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return X509_ERR_MALFORMED;
    memcpy(copy, der, len);
    user_roots[user_root_count].der = copy;
    user_roots[user_root_count].len = len;
    user_root_count++;
    return X509_OK;
}

/* ------------------------------------------------------ chain checking --- */

static int check_validity(const x509_cert_t *c, x509_time_t now)
{
    if (now.day == INT32_MIN) return X509_ERR_CLOCK;
    if (x509_time_cmp(now, c->not_before) < 0) return X509_ERR_NOT_YET_VALID;
    if (x509_time_cmp(now, c->not_after) > 0) return X509_ERR_EXPIRED;
    return X509_OK;
}

/* Can `ca` act as the issuer of a certificate with `below` CA certificates
 * between that certificate and itself? */
static int check_issuer_role(const x509_cert_t *ca, int below, x509_time_t now)
{
    if (ca->critical_unsupported) return X509_ERR_CRITICAL_EXT;
    if (!ca->has_bc || !ca->is_ca) return X509_ERR_NOT_CA;
    if (ca->has_ku && !(ca->ku & X509_KU_KEY_CERT_SIGN)) return X509_ERR_NOT_CA;
    if (ca->path_len >= 0 && below > ca->path_len) return X509_ERR_PATH_LEN;
    return check_validity(ca, now);
}

static int same_name(const uint8_t *a, uint32_t al, const uint8_t *b, uint32_t bl)
{
    return al == bl && memcmp(a, b, al) == 0;
}

int x509_verify_chain(const x509_der_t *chain, int n, const char *host,
                      x509_time_t now, int flags, x509_cert_t *leaf_out)
{
    if (n < 1) return X509_ERR_MALFORMED;
    if (n > X509_MAX_CHAIN) return X509_ERR_CHAIN_TOO_LONG;

    x509_cert_t c[X509_MAX_CHAIN];
    for (int i = 0; i < n; i++) {
        int rc = x509_parse(chain[i].der, chain[i].len, &c[i]);
        if (rc != X509_OK) return rc;
    }
    if (leaf_out) *leaf_out = c[0];

    int rc = check_validity(&c[0], now);
    if (rc != X509_OK) return rc;
    if (c[0].critical_unsupported) return X509_ERR_CRITICAL_EXT;
    if (!x509_host_matches(&c[0], host)) return X509_ERR_HOSTNAME;
    if (c[0].has_eku && !c[0].eku_server_auth) return X509_ERR_KEY_USAGE;
    if ((flags & X509_F_RSA_KEY_EXCHANGE) && c[0].has_ku && !(c[0].ku & X509_KU_KEY_ENCIPHERMENT)) return X509_ERR_KEY_USAGE;
    if ((flags & X509_F_SIGNATURE) && c[0].has_ku && !(c[0].ku & X509_KU_DIGITAL_SIGNATURE)) return X509_ERR_KEY_USAGE;

    uint32_t used = 1;
    int cur = 0, below = 0;
    int best = X509_ERR_UNKNOWN_ISSUER;

    for (int guard = 0; guard <= X509_MAX_CHAIN; guard++) {
        const x509_cert_t *cc = &c[cur];

        /* the leaf itself is a trust anchor (e.g. a pinned self-signed cert);
         * its validity was checked above. A copy of a root sent as part of the
         * chain gets no shortcut: it is judged by the anchor-issued test below,
         * with the same CA/path-length/validity rules as any other issuer. */
        if (cur == 0) {
            for (int i = 0; i < x509_trust_count(); i++) {
                x509_der_t r = x509_trust_get(i);
                if (r.len == cc->der_len && memcmp(r.der, cc->der, r.len) == 0) return X509_OK;
            }
        }

        /* issued by a trust anchor? */
        for (int i = 0; i < x509_trust_count(); i++) {
            x509_der_t r = x509_trust_get(i);
            x509_cert_t root;
            if (x509_parse(r.der, r.len, &root) != X509_OK) continue;
            if (!same_name(root.subject, root.subject_len, cc->issuer, cc->issuer_len)) continue;
            int er = check_issuer_role(&root, below, now);
            if (er == X509_OK) er = x509_check_signature(cc, &root);
            if (er == X509_OK) return X509_OK;
            if (best == X509_ERR_UNKNOWN_ISSUER) best = er;
        }

        /* otherwise find the issuer among the certificates the server sent */
        int next = -1;
        for (int k = 1; k < n; k++) {
            if (used & (1u << k)) continue;
            if (!same_name(c[k].subject, c[k].subject_len, cc->issuer, cc->issuer_len)) continue;
            int er = check_issuer_role(&c[k], below, now);
            if (er == X509_OK) er = x509_check_signature(cc, &c[k]);
            if (er == X509_OK) { next = k; break; }
            if (best == X509_ERR_UNKNOWN_ISSUER) best = er;
        }
        if (next < 0) return best;
        used |= 1u << next;
        cur = next;
        below++;
    }
    return X509_ERR_CHAIN_TOO_LONG;
}

const char *x509_strerror(int err)
{
    switch (err) {
    case X509_OK:                   return "ok";
    case X509_ERR_MALFORMED:        return "malformed certificate";
    case X509_ERR_UNSUPPORTED_KEY:  return "unsupported public key (RSA 2048-4096, or EC P-256/P-384)";
    case X509_ERR_SIG_ALG:          return "unsupported signature algorithm";
    case X509_ERR_WEAK_SIG_ALG:     return "weak (SHA-1/MD5) signature algorithm";
    case X509_ERR_BAD_SIGNATURE:    return "certificate signature is invalid";
    case X509_ERR_EXPIRED:          return "certificate has expired";
    case X509_ERR_NOT_YET_VALID:    return "certificate is not valid yet";
    case X509_ERR_HOSTNAME:         return "certificate is for a different host name";
    case X509_ERR_UNKNOWN_ISSUER:   return "certificate issuer is not trusted";
    case X509_ERR_NOT_CA:           return "issuer certificate is not a CA";
    case X509_ERR_PATH_LEN:         return "certificate path is longer than the CA allows";
    case X509_ERR_CRITICAL_EXT:     return "unsupported critical certificate extension";
    case X509_ERR_KEY_USAGE:        return "certificate may not be used for this purpose";
    case X509_ERR_CLOCK:            return "system clock is not set (cannot check validity)";
    case X509_ERR_CHAIN_TOO_LONG:   return "certificate chain too long";
    default:                        return "certificate error";
    }
}
