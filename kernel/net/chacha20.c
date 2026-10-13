#include "chacha20.h"
#include "string.h"
#include "memory.h"

/* -----------------------------------------------------------------------
 * ChaCha20 (RFC 7539)
 * ----------------------------------------------------------------------- */

#define ROTL32(v,n) (((v) << (n)) | ((v) >> (32-(n))))

#define QR(a,b,c,d) \
    a += b; d ^= a; d = ROTL32(d,16); \
    c += d; b ^= c; b = ROTL32(b,12); \
    a += b; d ^= a; d = ROTL32(d, 8); \
    c += d; b ^= c; b = ROTL32(b, 7);

static void chacha20_core(uint32_t out[16], const uint32_t in[16])
{
    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = in[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0],x[4],x[8], x[12])
        QR(x[1],x[5],x[9], x[13])
        QR(x[2],x[6],x[10],x[14])
        QR(x[3],x[7],x[11],x[15])
        QR(x[0],x[5],x[10],x[15])
        QR(x[1],x[6],x[11],x[12])
        QR(x[2],x[7],x[8], x[13])
        QR(x[3],x[4],x[9], x[14])
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

static uint32_t load32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static void store32_le(uint8_t *p, uint32_t v)
{
    p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF;
}

void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64])
{
    static const uint8_t sigma[16] = {'e','x','p','a','n','d',' ','3','2','-','b','y','t','e',' ','k'};
    uint32_t state[16];
    state[0]  = load32_le(sigma);
    state[1]  = load32_le(sigma+4);
    state[2]  = load32_le(sigma+8);
    state[3]  = load32_le(sigma+12);
    for (int i = 0; i < 8; i++) state[4+i] = load32_le(key + i*4);
    state[12] = counter;
    state[13] = load32_le(nonce);
    state[14] = load32_le(nonce+4);
    state[15] = load32_le(nonce+8);

    uint32_t block[16];
    chacha20_core(block, state);
    for (int i = 0; i < 16; i++) store32_le(out + i*4, block[i]);
}

void chacha20_encrypt(const uint8_t key[32], uint32_t counter,
                      const uint8_t nonce[12],
                      const uint8_t *in, uint8_t *out, int len)
{
    uint8_t block[64];
    int pos = 0;
    while (pos < len) {
        chacha20_block(key, counter++, nonce, block);
        int n = len - pos;
        if (n > 64) n = 64;
        for (int i = 0; i < n; i++) out[pos+i] = in[pos+i] ^ block[i];
        pos += n;
    }
}

/* -----------------------------------------------------------------------
 * HChaCha20 — derives 32-byte sub-key from first 16 bytes of a 24-byte nonce
 * ----------------------------------------------------------------------- */
void hchacha20(const uint8_t key[32], const uint8_t nonce16[16],
               uint8_t out[32])
{
    static const uint8_t sigma[16] = {'e','x','p','a','n','d',' ','3','2','-','b','y','t','e',' ','k'};
    uint32_t state[16];
    state[0]  = load32_le(sigma);
    state[1]  = load32_le(sigma+4);
    state[2]  = load32_le(sigma+8);
    state[3]  = load32_le(sigma+12);
    for (int i = 0; i < 8; i++) state[4+i] = load32_le(key + i*4);
    state[12] = load32_le(nonce16);
    state[13] = load32_le(nonce16+4);
    state[14] = load32_le(nonce16+8);
    state[15] = load32_le(nonce16+12);

    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = state[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0],x[4],x[8], x[12])
        QR(x[1],x[5],x[9], x[13])
        QR(x[2],x[6],x[10],x[14])
        QR(x[3],x[7],x[11],x[15])
        QR(x[0],x[5],x[10],x[15])
        QR(x[1],x[6],x[11],x[12])
        QR(x[2],x[7],x[8], x[13])
        QR(x[3],x[4],x[9], x[14])
    }
    /* output words 0-3 and 12-15 */
    store32_le(out,    x[0]);  store32_le(out+4,  x[1]);
    store32_le(out+8,  x[2]);  store32_le(out+12, x[3]);
    store32_le(out+16, x[12]); store32_le(out+20, x[13]);
    store32_le(out+24, x[14]); store32_le(out+28, x[15]);
}

void xchacha20_encrypt(const uint8_t key[32], const uint8_t nonce[24],
                       uint32_t counter,
                       const uint8_t *in, uint8_t *out, int len)
{
    uint8_t subkey[32];
    hchacha20(key, nonce, subkey);   /* nonce[0..15] -> subkey */
    /* Build a 12-byte ChaCha20 nonce: 4 zero bytes + nonce[16..23] */
    uint8_t cn[12];
    cn[0]=0; cn[1]=0; cn[2]=0; cn[3]=0;
    cn[4]=nonce[16]; cn[5]=nonce[17]; cn[6]=nonce[18]; cn[7]=nonce[19];
    cn[8]=nonce[20]; cn[9]=nonce[21]; cn[10]=nonce[22]; cn[11]=nonce[23];
    chacha20_encrypt(subkey, counter, cn, in, out, len);
}

/* -----------------------------------------------------------------------
 * Poly1305 (RFC 8439)
 * ----------------------------------------------------------------------- */

/* 130-bit accumulator using 5x26-bit limbs (all in 32-bit words) */
static void poly1305_mac_internal(const uint8_t key[32],
                                  const uint8_t *msg, int len,
                                  uint8_t tag[16])
{
    /* clamp r */
    uint32_t r[5], s[4];
    r[0] = (load32_le(key)    ) & 0x3FFFFFF;
    r[1] = (load32_le(key+3)  >> 2) & 0x3FFFF03;
    r[2] = (load32_le(key+6)  >> 4) & 0x3FFC0FF;
    r[3] = (load32_le(key+9)  >> 6) & 0x3F03FFF;
    r[4] = (load32_le(key+12) >> 8) & 0x00FFFFF;
    /* Clamp: r[1..4] already masked */
    r[1] &= 0x3FFFF03; r[2] &= 0x3FFC0FF; r[3] &= 0x3F03FFF; r[4] &= 0x00FFFFF;

    s[0] = load32_le(key+16); s[1] = load32_le(key+20);
    s[2] = load32_le(key+24); s[3] = load32_le(key+28);

    uint32_t h[5] = {0,0,0,0,0};

    while (len > 0) {
        uint8_t block[17];
        int n = len < 16 ? len : 16;
        memcpy(block, msg, n);
        block[n] = (n < 16) ? 1 : 0; /* high bit */
        if (n == 16) block[16] = 1;

        uint32_t m[5];
        m[0] = ((uint32_t)block[0]  | ((uint32_t)block[1]<<8)  | ((uint32_t)block[2]<<16) | ((uint32_t)block[3]<<24)) & 0x3FFFFFF;
        m[1] = (((uint32_t)block[3] | ((uint32_t)block[4]<<8)  | ((uint32_t)block[5]<<16) | ((uint32_t)block[6]<<24)) >> 2) & 0x3FFFFFF;
        m[2] = (((uint32_t)block[6] | ((uint32_t)block[7]<<8)  | ((uint32_t)block[8]<<16) | ((uint32_t)block[9]<<24)) >> 4) & 0x3FFFFFF;
        m[3] = (((uint32_t)block[9] | ((uint32_t)block[10]<<8) | ((uint32_t)block[11]<<16)| ((uint32_t)block[12]<<24))>> 6) & 0x3FFFFFF;
        m[4] = (((uint32_t)block[12]| ((uint32_t)block[13]<<8) | ((uint32_t)block[14]<<16)| ((uint32_t)block[15]<<24))>> 8) | ((uint32_t)block[16] << 24);

        for (int i = 0; i < 5; i++) h[i] += m[i];

        /* h = h * r  mod 2^130-5  (lazy reduction) */
        uint64_t d[5];
        uint32_t r1_5 = r[1]*5, r2_5 = r[2]*5, r3_5 = r[3]*5, r4_5 = r[4]*5;
        d[0] = (uint64_t)h[0]*r[0] + (uint64_t)h[1]*r4_5 + (uint64_t)h[2]*r3_5 + (uint64_t)h[3]*r2_5 + (uint64_t)h[4]*r1_5;
        d[1] = (uint64_t)h[0]*r[1] + (uint64_t)h[1]*r[0]  + (uint64_t)h[2]*r4_5 + (uint64_t)h[3]*r3_5 + (uint64_t)h[4]*r2_5;
        d[2] = (uint64_t)h[0]*r[2] + (uint64_t)h[1]*r[1]  + (uint64_t)h[2]*r[0]  + (uint64_t)h[3]*r4_5 + (uint64_t)h[4]*r3_5;
        d[3] = (uint64_t)h[0]*r[3] + (uint64_t)h[1]*r[2]  + (uint64_t)h[2]*r[1]  + (uint64_t)h[3]*r[0]  + (uint64_t)h[4]*r4_5;
        d[4] = (uint64_t)h[0]*r[4] + (uint64_t)h[1]*r[3]  + (uint64_t)h[2]*r[2]  + (uint64_t)h[3]*r[1]  + (uint64_t)h[4]*r[0];

        /* Propagate carries */
        uint32_t c;
        c = (uint32_t)(d[0] >> 26); h[0] = (uint32_t)d[0] & 0x3FFFFFF; d[1] += c;
        c = (uint32_t)(d[1] >> 26); h[1] = (uint32_t)d[1] & 0x3FFFFFF; d[2] += c;
        c = (uint32_t)(d[2] >> 26); h[2] = (uint32_t)d[2] & 0x3FFFFFF; d[3] += c;
        c = (uint32_t)(d[3] >> 26); h[3] = (uint32_t)d[3] & 0x3FFFFFF; d[4] += c;
        c = (uint32_t)(d[4] >> 26); h[4] = (uint32_t)d[4] & 0x3FFFFFF; h[0] += c * 5;
        c = h[0] >> 26; h[0] &= 0x3FFFFFF; h[1] += c;

        msg += n; len -= n;
    }

    /* Fully reduce h mod 2^130-5 */
    uint32_t c = h[1] >> 26; h[1] &= 0x3FFFFFF; h[2] += c;
              c = h[2] >> 26; h[2] &= 0x3FFFFFF; h[3] += c;
              c = h[3] >> 26; h[3] &= 0x3FFFFFF; h[4] += c;
              c = h[4] >> 26; h[4] &= 0x3FFFFFF; h[0] += c * 5;
              c = h[0] >> 26; h[0] &= 0x3FFFFFF; h[1] += c;

    /* Compare h with 2^130-5 */
    uint32_t g[5];
    g[0] = h[0] + 5; c = g[0] >> 26; g[0] &= 0x3FFFFFF;
    g[1] = h[1] + c; c = g[1] >> 26; g[1] &= 0x3FFFFFF;
    g[2] = h[2] + c; c = g[2] >> 26; g[2] &= 0x3FFFFFF;
    g[3] = h[3] + c; c = g[3] >> 26; g[3] &= 0x3FFFFFF;
    g[4] = h[4] + c - (1u << 26);
    /* select g if h >= 2^130-5 */
    uint32_t mask = (uint32_t)(-(int32_t)(g[4] >> 31) ^ 0xFFFFFFFF);
    h[0] = (h[0] & ~mask) | (g[0] & mask);
    h[1] = (h[1] & ~mask) | (g[1] & mask);
    h[2] = (h[2] & ~mask) | (g[2] & mask);
    h[3] = (h[3] & ~mask) | (g[3] & mask);
    h[4] = (h[4] & ~mask) | (g[4] & mask);

    /* h = (h + s) mod 2^128 */
    uint64_t f;
    f = (uint64_t)((h[0]     ) | (h[1]<<26)) + s[0]; uint32_t t0 = (uint32_t)f;
    f = (uint64_t)((h[1]>>6  ) | (h[2]<<20)) + s[1] + (f>>32); uint32_t t1 = (uint32_t)f;
    f = (uint64_t)((h[2]>>12 ) | (h[3]<<14)) + s[2] + (f>>32); uint32_t t2 = (uint32_t)f;
    f = (uint64_t)((h[3]>>18 ) | (h[4]<<8 )) + s[3] + (f>>32); uint32_t t3 = (uint32_t)f;

    store32_le(tag,    t0); store32_le(tag+4,  t1);
    store32_le(tag+8,  t2); store32_le(tag+12, t3);
}

void poly1305_mac(const uint8_t key[32],
                  const uint8_t *msg, int msg_len,
                  uint8_t tag[16])
{
    poly1305_mac_internal(key, msg, msg_len, tag);
}

/* -----------------------------------------------------------------------
 * XChaCha20-Poly1305 AEAD
 * ----------------------------------------------------------------------- */
void xchacha20poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[24],
                               const uint8_t *plain, int plain_len,
                               uint8_t *out)
{
    uint8_t subkey[32];
    hchacha20(key, nonce, subkey);
    const uint8_t *chacha_nonce = nonce + 16; /* 8 bytes, pad with 4 zero bytes */
    uint8_t cn[12];
    cn[0]=0; cn[1]=0; cn[2]=0; cn[3]=0;
    memcpy(cn+4, chacha_nonce, 8);

    /* Generate Poly1305 key from block 0 */
    uint8_t poly_block[64];
    chacha20_block(subkey, 0, cn, poly_block);

    /* Encrypt with counter=1 */
    chacha20_encrypt(subkey, 1, cn, plain, out, plain_len);

    /* Compute MAC over ciphertext */
    poly1305_mac_internal(poly_block, out, plain_len, out + plain_len);
}

int xchacha20poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[24],
                              const uint8_t *cipher, int cipher_len,
                              uint8_t *plain)
{
    if (cipher_len < 16) return -1;
    int plain_len = cipher_len - 16;

    uint8_t subkey[32];
    hchacha20(key, nonce, subkey);
    uint8_t cn[12];
    cn[0]=0; cn[1]=0; cn[2]=0; cn[3]=0;
    memcpy(cn+4, nonce+16, 8);

    uint8_t poly_block[64];
    chacha20_block(subkey, 0, cn, poly_block);

    /* Verify MAC */
    uint8_t expected[16];
    poly1305_mac_internal(poly_block, cipher, plain_len, expected);
    const uint8_t *received = cipher + plain_len;
    int diff = 0;
    for (int i = 0; i < 16; i++) diff |= (expected[i] ^ received[i]);
    if (diff) return -1;

    /* Decrypt */
    chacha20_encrypt(subkey, 1, cn, cipher, plain, plain_len);
    return 0;
}

/* ---- ChaCha20-Poly1305 AEAD (RFC 8439 section 2.8) ---- */

/* The one-time Poly1305 key is the first half of the keystream block at counter 0; the
 * data itself is encrypted from counter 1 on, so the two never overlap. */
static void cc20_poly_key(const uint8_t key[32], const uint8_t nonce[12], uint8_t otk[32])
{
    uint8_t blk[64];
    chacha20_block(key, 0, nonce, blk);
    memcpy(otk, blk, 32);
}

/* The MAC covers the AAD and the ciphertext, each padded to a multiple of 16, followed by
 * their two lengths -- the padding and the lengths are what stop one field being shifted
 * into the other without changing the tag. */
static void cc20_poly_tag(const uint8_t otk[32], const uint8_t *aad, int aad_len,
                          const uint8_t *cipher, int cipher_len, uint8_t tag[16])
{
    int ap = (16 - (aad_len % 16)) % 16;
    int cp = (16 - (cipher_len % 16)) % 16;
    int total = aad_len + ap + cipher_len + cp + 16;
    uint8_t *buf = (uint8_t *)malloc((size_t)total);
    if (!buf) { memset(tag, 0, 16); return; }
    int o = 0;
    if (aad_len) { memcpy(buf + o, aad, (size_t)aad_len); o += aad_len; }
    memset(buf + o, 0, (size_t)ap); o += ap;
    if (cipher_len) { memcpy(buf + o, cipher, (size_t)cipher_len); o += cipher_len; }
    memset(buf + o, 0, (size_t)cp); o += cp;
    for (int i = 0; i < 8; i++) buf[o + i] = (uint8_t)((uint64_t)aad_len >> (8 * i));
    for (int i = 0; i < 8; i++) buf[o + 8 + i] = (uint8_t)((uint64_t)cipher_len >> (8 * i));
    o += 16;
    poly1305_mac(otk, buf, o, tag);
    memset(buf, 0, (size_t)total);
    free(buf);
}

void chacha20poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                              const uint8_t *aad, int aad_len,
                              const uint8_t *plain, int plain_len,
                              uint8_t *cipher, uint8_t tag[16])
{
    uint8_t otk[32];
    cc20_poly_key(key, nonce, otk);
    if (plain_len) chacha20_encrypt(key, 1, nonce, plain, cipher, plain_len);
    cc20_poly_tag(otk, aad, aad_len, cipher, plain_len, tag);
    memset(otk, 0, sizeof(otk));
}

int chacha20poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                             const uint8_t *aad, int aad_len,
                             const uint8_t *cipher, int cipher_len,
                             uint8_t *plain, const uint8_t tag[16])
{
    uint8_t otk[32], want[16];
    cc20_poly_key(key, nonce, otk);
    cc20_poly_tag(otk, aad, aad_len, cipher, cipher_len, want);
    /* Checked before anything is decrypted, and in constant time. */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(want[i] ^ tag[i]);
    if (diff) { memset(otk, 0, sizeof(otk)); if (cipher_len) memset(plain, 0, (size_t)cipher_len); return -1; }
    if (cipher_len) chacha20_encrypt(key, 1, nonce, cipher, plain, cipher_len);
    memset(otk, 0, sizeof(otk));
    return 0;
}
