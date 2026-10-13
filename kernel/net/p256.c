#include "p256.h"
#include "string.h"

/*
 * P-256 arithmetic.
 *
 * Numbers are 8 limbs of 32 bits, least significant first. Multiplication modulo the
 * curve prime (and, for the signature arithmetic, modulo the group order) is done in
 * Montgomery form: the reduction then needs only multiplications and additions, which
 * keeps this free of the division code the kernel does not have.
 *
 * Only verification lives here, so every value is public and the code may branch on it.
 */

#define L 8                                  /* limbs in a 256-bit number */

typedef struct {
    uint32_t m[L];                           /* the modulus */
    uint32_t n0inv;                          /* -m^-1 mod 2^32, the Montgomery factor */
    uint32_t r2[L];                          /* R^2 mod m, for entering Montgomery form */
} mont_t;

static void bn_zero(uint32_t *a) { for (int i = 0; i < L; i++) a[i] = 0; }
static void bn_copy(uint32_t *a, const uint32_t *b) { for (int i = 0; i < L; i++) a[i] = b[i]; }
static int  bn_is_zero(const uint32_t *a) { uint32_t d = 0; for (int i = 0; i < L; i++) d |= a[i]; return d == 0; }

static int bn_cmp(const uint32_t *a, const uint32_t *b)
{
    for (int i = L - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    }
    return 0;
}

/* a -= b, returning the borrow out of the top limb. */
static uint32_t bn_sub(uint32_t *a, const uint32_t *b)
{
    uint64_t borrow = 0;
    for (int i = 0; i < L; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
    return (uint32_t)borrow;
}

/* a += b, returning the carry out of the top limb. */
static uint32_t bn_add(uint32_t *a, const uint32_t *b)
{
    uint64_t carry = 0;
    for (int i = 0; i < L; i++) {
        uint64_t t = (uint64_t)a[i] + b[i] + carry;
        a[i] = (uint32_t)t;
        carry = t >> 32;
    }
    return (uint32_t)carry;
}

static void bn_from_be(uint32_t *out, const uint8_t in[32])
{
    for (int i = 0; i < L; i++) {
        const uint8_t *p = in + (L - 1 - i) * 4;
        out[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
}

static void bn_to_be(uint8_t out[32], const uint32_t *in)
{
    for (int i = 0; i < L; i++) {
        uint8_t *p = out + (L - 1 - i) * 4;
        p[0] = (uint8_t)(in[i] >> 24); p[1] = (uint8_t)(in[i] >> 16);
        p[2] = (uint8_t)(in[i] >> 8);  p[3] = (uint8_t)in[i];
    }
}

/* r = a*b*R^-1 mod m, the Montgomery product (CIOS form). */
static void mont_mul(const mont_t *M, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint32_t t[L + 2];
    for (int i = 0; i < L + 2; i++) t[i] = 0;
    for (int i = 0; i < L; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < L; j++) {
            uint64_t s = (uint64_t)a[j] * b[i] + t[j] + carry;
            t[j] = (uint32_t)s;
            carry = s >> 32;
        }
        uint64_t s = (uint64_t)t[L] + carry;
        t[L] = (uint32_t)s;
        t[L + 1] = (uint32_t)(s >> 32);

        uint32_t u = t[0] * M->n0inv;         /* cancels the low limb */
        carry = 0;
        for (int j = 0; j < L; j++) {
            uint64_t v = (uint64_t)u * M->m[j] + t[j] + carry;
            t[j] = (uint32_t)v;
            carry = v >> 32;
        }
        s = (uint64_t)t[L] + carry;
        t[L] = (uint32_t)s;
        t[L + 1] += (uint32_t)(s >> 32);

        for (int j = 0; j <= L; j++) t[j] = t[j + 1];   /* shift down one limb */
        t[L + 1] = 0;
    }
    bn_copy(r, t);
    if (t[L] || bn_cmp(r, M->m) >= 0) bn_sub(r, M->m);
}

static void mont_enter(const mont_t *M, uint32_t *r, const uint32_t *a) { mont_mul(M, r, a, M->r2); }

static void mont_leave(const mont_t *M, uint32_t *r, const uint32_t *a)
{
    uint32_t one[L];
    bn_zero(one); one[0] = 1;
    mont_mul(M, r, a, one);
}

/* Builds the Montgomery parameters for a modulus, without needing a division:
 * n0inv by Newton iteration mod 2^32, R^2 by doubling 1 modulo m 512 times. */
static void mont_init(mont_t *M, const uint8_t modulus[32])
{
    bn_from_be(M->m, modulus);
    uint32_t inv = 1;
    for (int i = 0; i < 5; i++) inv *= 2 - M->m[0] * inv;   /* inv = m^-1 mod 2^32 */
    M->n0inv = (uint32_t)(0u - inv);

    uint32_t t[L];
    bn_zero(t); t[0] = 1;
    for (int i = 0; i < 2 * L * 32; i++) {
        uint32_t carry = bn_add(t, t);                     /* t = 2t mod m */
        if (carry || bn_cmp(t, M->m) >= 0) bn_sub(t, M->m);
    }
    bn_copy(M->r2, t);
}

/* Addition and subtraction modulo m (inputs already reduced). The second operand is
 * copied first so that these stay correct when the destination is also an input --
 * writing r before reading b would otherwise silently compute something else. */
static void mod_add(const mont_t *M, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint32_t t[L];
    bn_copy(t, b);
    bn_copy(r, a);
    uint32_t carry = bn_add(r, t);
    if (carry || bn_cmp(r, M->m) >= 0) bn_sub(r, M->m);
}

static void mod_sub(const mont_t *M, uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint32_t t[L];
    bn_copy(t, b);
    bn_copy(r, a);
    if (bn_sub(r, t)) bn_add(r, M->m);
}

/* r = a^e mod m, with both a and r in Montgomery form; e is a plain big-endian exponent. */
static void mont_pow(const mont_t *M, uint32_t *r, const uint32_t *a, const uint8_t e[32])
{
    uint32_t acc[L], base[L];
    bn_zero(acc);
    mont_enter(M, acc, (const uint32_t[L]){ 1, 0, 0, 0, 0, 0, 0, 0 });
    bn_copy(base, a);
    for (int byte = 0; byte < 32; byte++) {
        for (int bit = 7; bit >= 0; bit--) {
            mont_mul(M, acc, acc, acc);
            if ((e[byte] >> bit) & 1) mont_mul(M, acc, acc, base);
        }
    }
    bn_copy(r, acc);
}

/* ---------------- the curve ---------------- */

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
/* p-2 and n-2, the exponents that invert a value modulo each (Fermat). */
static const uint8_t P256_P_M2[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xfd };
static const uint8_t P256_N_M2[32] = {
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x4f };

/* A point in Jacobian coordinates: the affine point is (X/Z^2, Y/Z^3), which keeps the
 * inversions out of the inner loop. Z = 0 is the point at infinity. */
typedef struct { uint32_t X[L], Y[L], Z[L]; } jpoint_t;

typedef struct {
    mont_t fp;                 /* arithmetic modulo the curve prime */
    uint32_t b[L];             /* curve constant b, in Montgomery form */
    uint32_t one[L];           /* 1 in Montgomery form */
    jpoint_t G;
    int ready;
} curve_t;

static curve_t C;

static void curve_init(void)
{
    if (C.ready) return;
    mont_init(&C.fp, P256_P);
    uint32_t t[L];
    bn_zero(t); t[0] = 1;
    mont_enter(&C.fp, C.one, t);
    bn_from_be(t, P256_B);
    mont_enter(&C.fp, C.b, t);
    bn_from_be(t, P256_GX); mont_enter(&C.fp, C.G.X, t);
    bn_from_be(t, P256_GY); mont_enter(&C.fp, C.G.Y, t);
    bn_copy(C.G.Z, C.one);
    C.ready = 1;
}

static void jp_set_infinity(jpoint_t *p) { bn_copy(p->X, C.one); bn_copy(p->Y, C.one); bn_zero(p->Z); }
static int  jp_is_infinity(const jpoint_t *p) { return bn_is_zero(p->Z); }

/* Point doubling for a = -3 (which P-256 uses), in Montgomery form throughout. */
static void jp_double(jpoint_t *r, const jpoint_t *p)
{
    const mont_t *M = &C.fp;
    if (jp_is_infinity(p)) { jp_set_infinity(r); return; }
    uint32_t delta[L], gamma[L], beta[L], alpha[L], t1[L], t2[L];

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
static void jp_add(jpoint_t *r, const jpoint_t *p, const jpoint_t *q)
{
    const mont_t *M = &C.fp;
    if (jp_is_infinity(p)) { *r = *q; return; }
    if (jp_is_infinity(q)) { *r = *p; return; }
    uint32_t z1z1[L], z2z2[L], u1[L], u2[L], s1[L], s2[L], h[L], i[L], j[L], rr[L], v[L], t[L];

    mont_mul(M, z1z1, p->Z, p->Z);
    mont_mul(M, z2z2, q->Z, q->Z);
    mont_mul(M, u1, p->X, z2z2);
    mont_mul(M, u2, q->X, z1z1);
    mont_mul(M, s1, p->Y, q->Z); mont_mul(M, s1, s1, z2z2);
    mont_mul(M, s2, q->Y, p->Z); mont_mul(M, s2, s2, z1z1);

    if (bn_cmp(u1, u2) == 0) {
        if (bn_cmp(s1, s2) == 0) { jp_double(r, p); return; }   /* same point */
        jp_set_infinity(r);                                      /* p = -q */
        return;
    }
    mod_sub(M, h, u2, u1);
    mod_add(M, i, h, h); mont_mul(M, i, i, i);                   /* (2h)^2 */
    mont_mul(M, j, h, i);
    mod_sub(M, rr, s2, s1); mod_add(M, rr, rr, rr);              /* 2(s2-s1) */
    mont_mul(M, v, u1, i);

    mont_mul(M, t, rr, rr);
    mod_sub(M, t, t, j);
    mod_sub(M, t, t, v);
    mod_sub(M, r->X, t, v);                                      /* X = r^2 - j - 2v */

    mod_sub(M, t, v, r->X);
    mont_mul(M, t, rr, t);
    mont_mul(M, s1, s1, j);
    mod_add(M, s1, s1, s1);
    mod_sub(M, r->Y, t, s1);                                     /* Y = r(v-X) - 2*s1*j */

    mod_add(M, t, p->Z, q->Z);
    mont_mul(M, t, t, t);
    mod_sub(M, t, t, z1z1);
    mod_sub(M, t, t, z2z2);
    mont_mul(M, r->Z, t, h);                                     /* Z = ((Z1+Z2)^2 - z1z1 - z2z2)*h */
}

/* r = a*P + b*Q, the combination an ECDSA check needs. Both scalars are public, so the
 * simple interleaved double-and-add is fine here. */
static void jp_mul2(jpoint_t *r, const uint8_t a[32], const jpoint_t *P,
                    const uint8_t b[32], const jpoint_t *Q)
{
    jpoint_t sum;
    jp_add(&sum, P, Q);
    jp_set_infinity(r);
    for (int byte = 0; byte < 32; byte++) {
        for (int bit = 7; bit >= 0; bit--) {
            jp_double(r, r);
            int ab = ((a[byte] >> bit) & 1) | (((b[byte] >> bit) & 1) << 1);
            if (ab == 1) jp_add(r, r, P);
            else if (ab == 2) jp_add(r, r, Q);
            else if (ab == 3) jp_add(r, r, &sum);
        }
    }
}

/* The affine x of a Jacobian point, as a plain integer. */
static int jp_affine_x(uint32_t out[L], const jpoint_t *p)
{
    const mont_t *M = &C.fp;
    if (jp_is_infinity(p)) return -1;
    uint32_t zi[L], zi2[L], x[L];
    mont_pow(M, zi, p->Z, P256_P_M2);        /* Z^-1 */
    mont_mul(M, zi2, zi, zi);
    mont_mul(M, x, p->X, zi2);
    mont_leave(M, out, x);
    return 0;
}

/* Is (x, y) really a point of the curve? A "public key" off the curve can otherwise be
 * used to make a verification succeed for a signature the real key never produced. */
static int on_curve(const uint32_t x[L], const uint32_t y[L])
{
    const mont_t *M = &C.fp;
    uint32_t xm[L], ym[L], t[L], rhs[L], lhs[L], three[L];
    mont_enter(M, xm, x);
    mont_enter(M, ym, y);
    mont_mul(M, lhs, ym, ym);                        /* y^2 */
    mont_mul(M, rhs, xm, xm);
    mont_mul(M, rhs, rhs, xm);                       /* x^3 */
    mod_add(M, three, C.one, C.one);
    mod_add(M, three, three, C.one);
    mont_mul(M, t, three, xm);                       /* 3x */
    mod_sub(M, rhs, rhs, t);                         /* x^3 - 3x  (a = -3) */
    mod_add(M, rhs, rhs, C.b);
    return bn_cmp(lhs, rhs) == 0;
}

int p256_ecdsa_verify(const uint8_t pub[P256_PUBKEY_LEN],
                      const uint8_t r[32], const uint8_t s[32],
                      const uint8_t *digest, int digest_len)
{
    curve_init();
    if (pub[0] != 4) return -1;                      /* only the uncompressed form */

    mont_t fn;
    mont_init(&fn, P256_N);

    uint32_t rv[L], sv[L];
    bn_from_be(rv, r);
    bn_from_be(sv, s);
    if (bn_is_zero(rv) || bn_is_zero(sv)) return -1;
    if (bn_cmp(rv, fn.m) >= 0 || bn_cmp(sv, fn.m) >= 0) return -1;

    uint32_t qx[L], qy[L];
    bn_from_be(qx, pub + 1);
    bn_from_be(qy, pub + 33);
    if (bn_cmp(qx, C.fp.m) >= 0 || bn_cmp(qy, C.fp.m) >= 0) return -1;
    if (!on_curve(qx, qy)) return -1;

    /* e is the leftmost bits of the hash, so a longer digest is truncated rather than
     * reduced (FIPS 186-4 section 6.4). */
    uint8_t ebuf[32];
    if (digest_len >= 32) memcpy(ebuf, digest, 32);
    else { memset(ebuf, 0, 32); memcpy(ebuf + (32 - digest_len), digest, (size_t)digest_len); }
    uint32_t e[L];
    bn_from_be(e, ebuf);
    if (bn_cmp(e, fn.m) >= 0) bn_sub(e, fn.m);

    /* u1 = e/s, u2 = r/s, both modulo the group order */
    uint32_t sm[L], w[L], u1[L], u2[L], em[L], rm[L];
    mont_enter(&fn, sm, sv);
    mont_pow(&fn, w, sm, P256_N_M2);                 /* 1/s, in Montgomery form */
    mont_enter(&fn, em, e);
    mont_enter(&fn, rm, rv);
    mont_mul(&fn, u1, em, w); mont_leave(&fn, u1, u1);
    mont_mul(&fn, u2, rm, w); mont_leave(&fn, u2, u2);

    uint8_t u1b[32], u2b[32];
    bn_to_be(u1b, u1);
    bn_to_be(u2b, u2);

    jpoint_t Q;
    mont_enter(&C.fp, Q.X, qx);
    mont_enter(&C.fp, Q.Y, qy);
    bn_copy(Q.Z, C.one);

    jpoint_t R;
    jp_mul2(&R, u1b, &C.G, u2b, &Q);
    uint32_t x[L];
    if (jp_affine_x(x, &R) != 0) return -1;          /* the result was the point at infinity */
    if (bn_cmp(x, fn.m) >= 0) bn_sub(x, fn.m);
    return bn_cmp(x, rv) == 0 ? 0 : -1;
}
