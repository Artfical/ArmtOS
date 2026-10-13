#include "p256.h"
#include "string.h"

/*
 * Arithmetic for the two NIST curves certificates use, P-256 and P-384.
 *
 * Numbers are limbs of 32 bits, least significant first; a curve's limb count is carried
 * with its modulus, so one body of code serves both sizes. Multiplication modulo the curve
 * prime (and, for the signature arithmetic, modulo the group order) is done in Montgomery
 * form: the reduction then needs only multiplications and additions, which keeps this free
 * of the division code the kernel does not have.
 *
 * Only verification lives here, so every value is public and the code may branch on it.
 */

#define MAXL 12                              /* limbs in the largest number here (384 bits) */

typedef struct {
    int      n;                              /* limbs in this modulus */
    uint32_t m[MAXL];                        /* the modulus */
    uint32_t n0inv;                          /* -m^-1 mod 2^32, the Montgomery factor */
    uint32_t r2[MAXL];                       /* R^2 mod m, for entering Montgomery form */
} mont_t;

static void bn_zero(uint32_t *a, int n) { for (int i = 0; i < n; i++) a[i] = 0; }
static void bn_copy(uint32_t *a, const uint32_t *b, int n) { for (int i = 0; i < n; i++) a[i] = b[i]; }
static int  bn_is_zero(const uint32_t *a, int n) { uint32_t d = 0; for (int i = 0; i < n; i++) d |= a[i]; return d == 0; }

static int bn_cmp(const uint32_t *a, const uint32_t *b, int n)
{
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}

/* a -= b, returning the borrow out of the top limb. */
static uint32_t bn_sub(uint32_t *a, const uint32_t *b, int n)
{
    uint64_t borrow = 0;
    for (int i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
    return (uint32_t)borrow;
}

/* a += b, returning the carry out of the top limb. */
static uint32_t bn_add(uint32_t *a, const uint32_t *b, int n)
{
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t t = (uint64_t)a[i] + b[i] + carry;
        a[i] = (uint32_t)t;
        carry = t >> 32;
    }
    return (uint32_t)carry;
}

static void bn_from_be(uint32_t *out, const uint8_t *in, int n)
{
    for (int i = 0; i < n; i++) {
        const uint8_t *p = in + (n - 1 - i) * 4;
        out[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
}

static void bn_to_be(uint8_t *out, const uint32_t *in, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t *p = out + (n - 1 - i) * 4;
        p[0] = (uint8_t)(in[i] >> 24); p[1] = (uint8_t)(in[i] >> 16);
        p[2] = (uint8_t)(in[i] >> 8);  p[3] = (uint8_t)in[i];
    }
}

/* r = a*b*R^-1 mod m, the Montgomery product (CIOS form). */
static void mont_mul(const mont_t *M, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    int n = M->n;
    uint32_t t[MAXL + 2];
    for (int i = 0; i < n + 2; i++) t[i] = 0;
    for (int i = 0; i < n; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < n; j++) {
            uint64_t s = (uint64_t)a[j] * b[i] + t[j] + carry;
            t[j] = (uint32_t)s;
            carry = s >> 32;
        }
        uint64_t s = (uint64_t)t[n] + carry;
        t[n] = (uint32_t)s;
        t[n + 1] = (uint32_t)(s >> 32);

        uint32_t u = t[0] * M->n0inv;         /* cancels the low limb */
        carry = 0;
        for (int j = 0; j < n; j++) {
            uint64_t v = (uint64_t)u * M->m[j] + t[j] + carry;
            t[j] = (uint32_t)v;
            carry = v >> 32;
        }
        s = (uint64_t)t[n] + carry;
        t[n] = (uint32_t)s;
        t[n + 1] += (uint32_t)(s >> 32);

        for (int j = 0; j <= n; j++) t[j] = t[j + 1];   /* shift down one limb */
        t[n + 1] = 0;
    }
    bn_copy(r, t, n);
    if (t[n] || bn_cmp(r, M->m, n) >= 0) bn_sub(r, M->m, n);
}

static void mont_enter(const mont_t *M, uint32_t *r, const uint32_t *a) { mont_mul(M, r, a, M->r2); }

static void mont_leave(const mont_t *M, uint32_t *r, const uint32_t *a)
{
    uint32_t one[MAXL];
    bn_zero(one, M->n); one[0] = 1;
    mont_mul(M, r, a, one);
}

/* Builds the Montgomery parameters for a modulus, without needing a division:
 * n0inv by Newton iteration mod 2^32, R^2 by doubling 1 modulo m 2*n*32 times. */
static void mont_init(mont_t *M, const uint8_t *modulus, int n)
{
    M->n = n;
    bn_from_be(M->m, modulus, n);
    uint32_t inv = 1;
    for (int i = 0; i < 5; i++) inv *= 2 - M->m[0] * inv;   /* inv = m^-1 mod 2^32 */
    M->n0inv = (uint32_t)(0u - inv);

    uint32_t t[MAXL];
    bn_zero(t, n); t[0] = 1;
    for (int i = 0; i < 2 * n * 32; i++) {
        uint32_t carry = bn_add(t, t, n);                  /* t = 2t mod m */
        if (carry || bn_cmp(t, M->m, n) >= 0) bn_sub(t, M->m, n);
    }
    bn_copy(M->r2, t, n);
}

/* Addition and subtraction modulo m (inputs already reduced). The second operand is
 * copied first so that these stay correct when the destination is also an input --
 * writing r before reading b would otherwise silently compute something else. */
static void mod_add(const mont_t *M, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint32_t t[MAXL];
    bn_copy(t, b, M->n);
    bn_copy(r, a, M->n);
    uint32_t carry = bn_add(r, t, M->n);
    if (carry || bn_cmp(r, M->m, M->n) >= 0) bn_sub(r, M->m, M->n);
}

static void mod_sub(const mont_t *M, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint32_t t[MAXL];
    bn_copy(t, b, M->n);
    bn_copy(r, a, M->n);
    if (bn_sub(r, t, M->n)) bn_add(r, M->m, M->n);
}

/* r = a^(m-2) mod m: the inverse, by Fermat. Both a and r are in Montgomery form, and the
 * exponent is derived from the modulus itself rather than stored as another constant. */
static void mont_inv(const mont_t *M, uint32_t *r, const uint32_t *a)
{
    int n = M->n;
    uint8_t e[MAXL * 4];
    uint32_t em[MAXL];
    bn_zero(em, MAXL);
    bn_copy(em, M->m, n);
    uint32_t two[MAXL];
    bn_zero(two, n); two[0] = 2;
    bn_sub(em, two, n);                       /* m - 2 */
    bn_to_be(e, em, n);

    uint32_t acc[MAXL], base[MAXL], one[MAXL];
    bn_zero(one, n); one[0] = 1;
    mont_enter(M, acc, one);
    bn_copy(base, a, n);
    for (int byte = 0; byte < n * 4; byte++) {
        for (int bit = 7; bit >= 0; bit--) {
            mont_mul(M, acc, acc, acc);
            if ((e[byte] >> bit) & 1) mont_mul(M, acc, acc, base);
        }
    }
    bn_copy(r, acc, n);
}

/* ---------------- the curves ---------------- */

static const uint8_t P256_P[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff };
static const uint8_t P256_N[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51 };
static const uint8_t P256_B[32] = {
    0x5a,0xc6,0x35,0xd8,0xaa,0x3a,0x93,0xe7,0xb3,0xeb,0xbd,0x55,0x76,0x98,0x86,0xbc,
    0x65,0x1d,0x06,0xb0,0xcc,0x53,0xb0,0xf6,0x3b,0xce,0x3c,0x3e,0x27,0xd2,0x60,0x4b };
static const uint8_t P256_GX[32] = {
    0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96 };
static const uint8_t P256_GY[32] = {
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5 };

static const uint8_t P384_P[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfe,
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff };
static const uint8_t P384_N[48] = {
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xc7,0x63,0x4d,0x81,0xf4,0x37,0x2d,0xdf,
    0x58,0x1a,0x0d,0xb2,0x48,0xb0,0xa7,0x7a,0xec,0xec,0x19,0x6a,0xcc,0xc5,0x29,0x73 };
static const uint8_t P384_B[48] = {
    0xb3,0x31,0x2f,0xa7,0xe2,0x3e,0xe7,0xe4,0x98,0x8e,0x05,0x6b,0xe3,0xf8,0x2d,0x19,
    0x18,0x1d,0x9c,0x6e,0xfe,0x81,0x41,0x12,0x03,0x14,0x08,0x8f,0x50,0x13,0x87,0x5a,
    0xc6,0x56,0x39,0x8d,0x8a,0x2e,0xd1,0x9d,0x2a,0x85,0xc8,0xed,0xd3,0xec,0x2a,0xef };
static const uint8_t P384_GX[48] = {
    0xaa,0x87,0xca,0x22,0xbe,0x8b,0x05,0x37,0x8e,0xb1,0xc7,0x1e,0xf3,0x20,0xad,0x74,
    0x6e,0x1d,0x3b,0x62,0x8b,0xa7,0x9b,0x98,0x59,0xf7,0x41,0xe0,0x82,0x54,0x2a,0x38,
    0x55,0x02,0xf2,0x5d,0xbf,0x55,0x29,0x6c,0x3a,0x54,0x5e,0x38,0x72,0x76,0x0a,0xb7 };
static const uint8_t P384_GY[48] = {
    0x36,0x17,0xde,0x4a,0x96,0x26,0x2c,0x6f,0x5d,0x9e,0x98,0xbf,0x92,0x92,0xdc,0x29,
    0xf8,0xf4,0x1d,0xbd,0x28,0x9a,0x14,0x7c,0xe9,0xda,0x31,0x13,0xb5,0xf0,0xb8,0xc0,
    0x0a,0x60,0xb1,0xce,0x1d,0x7e,0x81,0x9d,0x7a,0x43,0x1d,0x7c,0x90,0xea,0x0e,0x5f };

/* A point in Jacobian coordinates: the affine point is (X/Z^2, Y/Z^3), which keeps the
 * inversions out of the inner loop. Z = 0 is the point at infinity. */
typedef struct { uint32_t X[MAXL], Y[MAXL], Z[MAXL]; } jpoint_t;

typedef struct {
    int      ready;
    int      n;                /* limbs */
    mont_t   fp;               /* arithmetic modulo the curve prime */
    mont_t   fn;               /* arithmetic modulo the group order */
    uint32_t b[MAXL];          /* curve constant b, in Montgomery form */
    uint32_t one[MAXL];        /* 1 in Montgomery form */
    jpoint_t G;
} curve_t;

static curve_t curves[2];      /* [0] = P-256, [1] = P-384 */

int ec_curve_coord_len(int curve)
{
    return curve == EC_CURVE_P256 ? 32 : curve == EC_CURVE_P384 ? 48 : 0;
}

static curve_t *curve_get(int curve)
{
    int idx = curve == EC_CURVE_P256 ? 0 : curve == EC_CURVE_P384 ? 1 : -1;
    if (idx < 0) return 0;
    curve_t *C = &curves[idx];
    if (C->ready) return C;

    const uint8_t *p, *nn, *b, *gx, *gy;
    int n;
    if (idx == 0) { p = P256_P; nn = P256_N; b = P256_B; gx = P256_GX; gy = P256_GY; n = 8; }
    else          { p = P384_P; nn = P384_N; b = P384_B; gx = P384_GX; gy = P384_GY; n = 12; }

    C->n = n;
    mont_init(&C->fp, p, n);
    mont_init(&C->fn, nn, n);
    uint32_t t[MAXL];
    bn_zero(t, n); t[0] = 1;
    mont_enter(&C->fp, C->one, t);
    bn_from_be(t, b, n);  mont_enter(&C->fp, C->b, t);
    bn_from_be(t, gx, n); mont_enter(&C->fp, C->G.X, t);
    bn_from_be(t, gy, n); mont_enter(&C->fp, C->G.Y, t);
    bn_copy(C->G.Z, C->one, n);
    C->ready = 1;
    return C;
}

static void jp_set_infinity(const curve_t *C, jpoint_t *p)
{
    bn_copy(p->X, C->one, C->n); bn_copy(p->Y, C->one, C->n); bn_zero(p->Z, C->n);
}
static int jp_is_infinity(const curve_t *C, const jpoint_t *p) { return bn_is_zero(p->Z, C->n); }

/* Point doubling for a = -3, which both curves use. */
static void jp_double(const curve_t *C, jpoint_t *r, const jpoint_t *p)
{
    const mont_t *M = &C->fp;
    if (jp_is_infinity(C, p)) { jp_set_infinity(C, r); return; }
    uint32_t delta[MAXL], gamma[MAXL], beta[MAXL], alpha[MAXL], t1[MAXL], t2[MAXL];

    mont_mul(M, delta, p->Z, p->Z);                 /* delta = Z^2 */
    mont_mul(M, gamma, p->Y, p->Y);                 /* gamma = Y^2 */
    mont_mul(M, beta, p->X, gamma);                 /* beta  = X*gamma */

    mod_sub(M, t1, p->X, delta);
    mod_add(M, t2, p->X, delta);
    mont_mul(M, alpha, t1, t2);                     /* (X-delta)(X+delta) */
    mod_add(M, t1, alpha, alpha);
    mod_add(M, alpha, t1, alpha);                   /* alpha = 3(X-delta)(X+delta) */

    mont_mul(M, t1, alpha, alpha);
    mod_add(M, t2, beta, beta);
    mod_add(M, t2, t2, t2);                         /* 4*beta */
    mod_sub(M, r->X, t1, t2);
    mod_sub(M, r->X, r->X, t2);                     /* X' = alpha^2 - 8*beta */

    mod_add(M, t1, p->Y, p->Z);
    mont_mul(M, t1, t1, t1);
    mod_sub(M, t1, t1, gamma);
    mod_sub(M, r->Z, t1, delta);                    /* Z' = (Y+Z)^2 - gamma - delta */

    mod_sub(M, t1, t2, r->X);                       /* 4*beta - X' */
    mont_mul(M, t1, alpha, t1);
    mont_mul(M, t2, gamma, gamma);
    mod_add(M, t2, t2, t2);
    mod_add(M, t2, t2, t2);
    mod_add(M, t2, t2, t2);                         /* 8*gamma^2 */
    mod_sub(M, r->Y, t1, t2);
}

/* r = p + q, both in Jacobian form. */
static void jp_add(const curve_t *C, jpoint_t *r, const jpoint_t *p, const jpoint_t *q)
{
    const mont_t *M = &C->fp;
    int n = C->n;
    if (jp_is_infinity(C, p)) { *r = *q; return; }
    if (jp_is_infinity(C, q)) { *r = *p; return; }
    uint32_t z1z1[MAXL], z2z2[MAXL], u1[MAXL], u2[MAXL], s1[MAXL], s2[MAXL];
    uint32_t h[MAXL], i[MAXL], j[MAXL], rr[MAXL], v[MAXL], t[MAXL];

    mont_mul(M, z1z1, p->Z, p->Z);
    mont_mul(M, z2z2, q->Z, q->Z);
    mont_mul(M, u1, p->X, z2z2);
    mont_mul(M, u2, q->X, z1z1);
    mont_mul(M, s1, p->Y, q->Z); mont_mul(M, s1, s1, z2z2);
    mont_mul(M, s2, q->Y, p->Z); mont_mul(M, s2, s2, z1z1);

    if (bn_cmp(u1, u2, n) == 0) {
        if (bn_cmp(s1, s2, n) == 0) { jp_double(C, r, p); return; }   /* the same point */
        jp_set_infinity(C, r);                                         /* p = -q */
        return;
    }
    mod_sub(M, h, u2, u1);
    mod_add(M, i, h, h); mont_mul(M, i, i, i);                         /* (2h)^2 */
    mont_mul(M, j, h, i);
    mod_sub(M, rr, s2, s1); mod_add(M, rr, rr, rr);                    /* 2(s2-s1) */
    mont_mul(M, v, u1, i);

    mont_mul(M, t, rr, rr);
    mod_sub(M, t, t, j);
    mod_sub(M, t, t, v);
    mod_sub(M, r->X, t, v);                                            /* X = r^2 - j - 2v */

    mod_sub(M, t, v, r->X);
    mont_mul(M, t, rr, t);
    mont_mul(M, s1, s1, j);
    mod_add(M, s1, s1, s1);
    mod_sub(M, r->Y, t, s1);                                           /* Y = r(v-X) - 2*s1*j */

    mod_add(M, t, p->Z, q->Z);
    mont_mul(M, t, t, t);
    mod_sub(M, t, t, z1z1);
    mod_sub(M, t, t, z2z2);
    mont_mul(M, r->Z, t, h);                                           /* Z = ((Z1+Z2)^2 - z1z1 - z2z2)*h */
}

/* r = a*P + b*Q, the combination an ECDSA check needs. Both scalars are public, so the
 * simple interleaved double-and-add is fine here. */
static void jp_mul2(const curve_t *C, jpoint_t *r, const uint8_t *a, const jpoint_t *P,
                    const uint8_t *b, const jpoint_t *Q, int len)
{
    jpoint_t sum;
    jp_add(C, &sum, P, Q);
    jp_set_infinity(C, r);
    for (int byte = 0; byte < len; byte++) {
        for (int bit = 7; bit >= 0; bit--) {
            jp_double(C, r, r);
            int ab = ((a[byte] >> bit) & 1) | (((b[byte] >> bit) & 1) << 1);
            if (ab == 1) jp_add(C, r, r, P);
            else if (ab == 2) jp_add(C, r, r, Q);
            else if (ab == 3) jp_add(C, r, r, &sum);
        }
    }
}

/* The affine x of a Jacobian point, as a plain integer. */
static int jp_affine_x(const curve_t *C, uint32_t *out, const jpoint_t *p)
{
    const mont_t *M = &C->fp;
    if (jp_is_infinity(C, p)) return -1;
    uint32_t zi[MAXL], zi2[MAXL], x[MAXL];
    mont_inv(M, zi, p->Z);
    mont_mul(M, zi2, zi, zi);
    mont_mul(M, x, p->X, zi2);
    mont_leave(M, out, x);
    return 0;
}

/* Is (x, y) really a point of the curve? A "public key" off the curve can otherwise be
 * used to make a verification succeed for a signature the real key never produced. */
static int on_curve(const curve_t *C, const uint32_t *x, const uint32_t *y)
{
    const mont_t *M = &C->fp;
    uint32_t xm[MAXL], ym[MAXL], t[MAXL], rhs[MAXL], lhs[MAXL], three[MAXL];
    mont_enter(M, xm, x);
    mont_enter(M, ym, y);
    mont_mul(M, lhs, ym, ym);                        /* y^2 */
    mont_mul(M, rhs, xm, xm);
    mont_mul(M, rhs, rhs, xm);                       /* x^3 */
    mod_add(M, three, C->one, C->one);
    mod_add(M, three, three, C->one);
    mont_mul(M, t, three, xm);                       /* 3x */
    mod_sub(M, rhs, rhs, t);                         /* x^3 - 3x  (a = -3) */
    mod_add(M, rhs, rhs, C->b);
    return bn_cmp(lhs, rhs, C->n) == 0;
}

int ec_ecdsa_verify(int curve, const uint8_t *pub, uint32_t pub_len,
                    const uint8_t *r, const uint8_t *s,
                    const uint8_t *digest, int digest_len)
{
    curve_t *C = curve_get(curve);
    int clen = ec_curve_coord_len(curve);
    if (!C || clen == 0) return -1;
    if (pub_len != (uint32_t)(1 + 2 * clen) || pub[0] != 4) return -1;   /* only the uncompressed form */
    int n = C->n;

    uint32_t rv[MAXL], sv[MAXL];
    bn_from_be(rv, r, n);
    bn_from_be(sv, s, n);
    if (bn_is_zero(rv, n) || bn_is_zero(sv, n)) return -1;
    if (bn_cmp(rv, C->fn.m, n) >= 0 || bn_cmp(sv, C->fn.m, n) >= 0) return -1;

    uint32_t qx[MAXL], qy[MAXL];
    bn_from_be(qx, pub + 1, n);
    bn_from_be(qy, pub + 1 + clen, n);
    if (bn_cmp(qx, C->fp.m, n) >= 0 || bn_cmp(qy, C->fp.m, n) >= 0) return -1;
    if (!on_curve(C, qx, qy)) return -1;

    /* e is the leftmost bits of the hash, so a longer digest is truncated rather than
     * reduced (FIPS 186-4 section 6.4). */
    uint8_t ebuf[EC_MAX_COORD_LEN];
    if (digest_len >= clen) memcpy(ebuf, digest, (size_t)clen);
    else { memset(ebuf, 0, (size_t)clen); memcpy(ebuf + (clen - digest_len), digest, (size_t)digest_len); }
    uint32_t e[MAXL];
    bn_from_be(e, ebuf, n);
    if (bn_cmp(e, C->fn.m, n) >= 0) bn_sub(e, C->fn.m, n);

    /* u1 = e/s, u2 = r/s, both modulo the group order */
    uint32_t sm[MAXL], w[MAXL], u1[MAXL], u2[MAXL], em[MAXL], rm[MAXL];
    mont_enter(&C->fn, sm, sv);
    mont_inv(&C->fn, w, sm);                         /* 1/s, in Montgomery form */
    mont_enter(&C->fn, em, e);
    mont_enter(&C->fn, rm, rv);
    mont_mul(&C->fn, u1, em, w); mont_leave(&C->fn, u1, u1);
    mont_mul(&C->fn, u2, rm, w); mont_leave(&C->fn, u2, u2);

    uint8_t u1b[EC_MAX_COORD_LEN], u2b[EC_MAX_COORD_LEN];
    bn_to_be(u1b, u1, n);
    bn_to_be(u2b, u2, n);

    jpoint_t Q;
    mont_enter(&C->fp, Q.X, qx);
    mont_enter(&C->fp, Q.Y, qy);
    bn_copy(Q.Z, C->one, n);

    jpoint_t R;
    jp_mul2(C, &R, u1b, &C->G, u2b, &Q, clen);
    uint32_t x[MAXL];
    if (jp_affine_x(C, x, &R) != 0) return -1;       /* the result was the point at infinity */
    if (bn_cmp(x, C->fn.m, n) >= 0) bn_sub(x, C->fn.m, n);
    return bn_cmp(x, rv, n) == 0 ? 0 : -1;
}
