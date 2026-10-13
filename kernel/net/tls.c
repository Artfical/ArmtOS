#include "tls.h"
#include "csprng.h"
#include "http.h"
#include "tcp.h"
#include "arp.h"
#include "sha256.h"
#include "sha512.h"
#include "aes.h"
#include "gcm.h"
#include "x25519.h"
#include "p256.h"
#include "x509.h"
#include "rsa.h"
#include "cmos.h"
#include "string.h"
#include "memory.h"
#include "klog.h"

/* TLS 1.2 client. Two cipher suites are offered, best first:
 *
 *   TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (0xC02F) -- ephemeral X25519 key exchange, AES-GCM
 *   TLS_RSA_WITH_AES_128_CBC_SHA256       (0x003C) -- the older RSA key transport, kept as a fallback
 *
 * The first is what modern servers expect, and the one that gives forward secrecy: the session
 * key comes from a key exchange thrown away afterwards, so someone who later obtains the server's
 * private key still cannot decrypt recorded traffic. The server certificate chain is validated
 * against the built-in CA store (see x509.c / ca_store.c) unless tls_set_verify(0) was called. */

#define TLS_VER_MAJOR 3
#define TLS_VER_MINOR 3   /* TLS 1.2 */
#define TLS_RT_HANDSHAKE       22
#define TLS_RT_CHANGE_CIPHER   20
#define TLS_RT_ALERT           21
#define TLS_RT_DATA            23
#define TLS_HT_CLIENT_HELLO    1
#define TLS_HT_SERVER_HELLO    2
#define TLS_HT_CERTIFICATE     11
#define TLS_HT_SERVER_KEY_EX   12
#define TLS_HT_CERT_REQUEST    13
#define TLS_HT_SERVER_DONE     14
#define TLS_HT_CLIENT_KEY_EX   16
#define TLS_HT_FINISHED        20
#define TLS_CIPHER_RSA_AES128_CBC_SHA256  0x003C
#define TLS_CIPHER_ECDHE_RSA_AES128_GCM   0xC02F
#define TLS_CIPHER_ECDHE_ECDSA_AES128_GCM 0xC02B
#define TLS_GROUP_X25519                  0x001D
#define TLS_GROUP_SECP256R1               0x0017
#define TLS_GCM_FIXED_IV   4              /* the salt half of the nonce, from the key block */
#define TLS_GCM_EXPLICIT   8              /* the per-record half, sent in the clear */
#define TLS_GCM_TAG       16

static int suite_is_gcm(const tls_ctx_t *ctx)
{
    return ctx->suite == TLS_CIPHER_ECDHE_RSA_AES128_GCM || ctx->suite == TLS_CIPHER_ECDHE_ECDSA_AES128_GCM;
}
static int suite_is_ecdsa(const tls_ctx_t *ctx) { return ctx->suite == TLS_CIPHER_ECDHE_ECDSA_AES128_GCM; }

/* All TLS randomness (client random, premaster secret, CBC IVs, RSA padding)
 * comes from the kernel CSPRNG. This used to be an LCG with the constant seed
 * 0xdeadbeef, which made every one of those values predictable. */
static void prng_fill(uint8_t *buf, int len)
{
    csprng_fill(buf, len);
}

/* ---- Record layer helpers ---- */

/* memset() that the compiler may not drop as a dead store. */
static void secure_zero(void *p, size_t n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;
    while (n--) *q++ = 0;
}

/* Erases everything that protects (or contains) the connection's data. */
static void tls_wipe_keys(tls_ctx_t *ctx)
{
    secure_zero(ctx->master, sizeof(ctx->master));
    secure_zero(ctx->client_write_key, sizeof(ctx->client_write_key));
    secure_zero(ctx->server_write_key, sizeof(ctx->server_write_key));
    secure_zero(ctx->client_write_iv, sizeof(ctx->client_write_iv));
    secure_zero(ctx->server_write_iv, sizeof(ctx->server_write_iv));
    secure_zero(ctx->client_mac, sizeof(ctx->client_mac));
    secure_zero(ctx->server_mac, sizeof(ctx->server_mac));
    secure_zero(ctx->rx_plain, sizeof(ctx->rx_plain));
    ctx->rx_plain_len = ctx->rx_plain_pos = 0;
}

/* Comparison whose running time does not depend on where the bytes differ. */
static int ct_equal(const uint8_t *a, const uint8_t *b, int n)
{
    uint8_t d = 0;
    for (int i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}


/* A TLS ECDSA signature is the same DER SEQUENCE { INTEGER r, INTEGER s } a certificate
 * carries; both integers are laid out as fixed 32-byte values for the curve code. */
static int ecdsa_sig_to_rs(const uint8_t *sig, uint32_t len, uint8_t r[32], uint8_t s[32])
{
    if (len < 8 || sig[0] != 0x30) return -1;
    uint32_t off = 2, seq_len = sig[1];
    if (seq_len & 0x80) {                       /* long form: one length byte is all a P-256 signature needs */
        if ((seq_len & 0x7F) != 1 || len < 3) return -1;
        seq_len = sig[2];
        off = 3;
    }
    if (off + seq_len != len) return -1;
    uint8_t *dst[2] = { r, s };
    for (int i = 0; i < 2; i++) {
        if (off + 2 > len || sig[off] != 0x02) return -1;
        uint32_t il = sig[off + 1];
        off += 2;
        if (il == 0 || il > 33 || off + il > len) return -1;
        const uint8_t *iv = sig + off;
        off += il;
        if (iv[0] & 0x80) return -1;            /* negative */
        while (il > 1 && iv[0] == 0) { iv++; il--; }
        if (il > 32) return -1;
        memset(dst[i], 0, 32);
        memcpy(dst[i] + (32 - il), iv, il);
    }
    return off == len ? 0 : -1;
}

static uint16_t u16be(const uint8_t *p) { return ((uint16_t)p[0]<<8)|p[1]; }
static uint32_t u24be(const uint8_t *p) { return ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2]; }
static uint32_t u32be(const uint8_t *p)
{
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

static void put_u8 (uint8_t *p, uint8_t  v) { p[0]=v; }
static void __attribute__((unused)) put_u16(uint8_t *p, uint16_t v) { p[0]=(v>>8)&0xFF; p[1]=v&0xFF; }
static void put_u24(uint8_t *p, uint32_t v) { p[0]=(v>>16)&0xFF; p[1]=(v>>8)&0xFF; p[2]=v&0xFF; }

/* Send a raw TLS record */
static int tls_send_raw(tls_ctx_t *ctx, uint8_t type, const uint8_t *data, int len)
{
    uint8_t hdr[5];
    hdr[0]=type; hdr[1]=TLS_VER_MAJOR; hdr[2]=TLS_VER_MINOR;
    hdr[3]=(uint8_t)(len>>8); hdr[4]=(uint8_t)(len);
    if (tcp_send2(ctx->fd, hdr, 5) != 0) return -1;
    if (len > 0 && tcp_send2(ctx->fd, (void*)data, len) != 0) return -1;
    return 0;
}

/* Append to handshake transcript */
static void hs_append(tls_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    sha256_update(&ctx->hs_hash, data, len);
}

/* Hash of everything appended so far (the running state is left untouched). */
static void hs_hash_now(const tls_ctx_t *ctx, uint8_t out[32])
{
    sha256_t t = ctx->hs_hash;
    sha256_final(&t, out);
}

/* Send a handshake message (type + 3-byte length + body), also records transcript */
static int tls_send_hs(tls_ctx_t *ctx, uint8_t hs_type, const uint8_t *body, int blen)
{
    uint8_t hdr[4];
    hdr[0] = hs_type;
    put_u24(hdr+1, (uint32_t)blen);
    hs_append(ctx, hdr, 4);
    hs_append(ctx, body, (uint32_t)blen);
    /* wrap in record */
    uint8_t rec[8192];
    if (4 + blen > (int)sizeof(rec)) return -1;
    rec[0]=hs_type; put_u24(rec+1,(uint32_t)blen);
    memcpy(rec+4, body, (uint32_t)blen);
    return tls_send_raw(ctx, TLS_RT_HANDSHAKE, rec, 4+blen);
}

/* ---- TLS PRF (RFC 5246): P_SHA256 ---- */
static void prf_p_sha256(const uint8_t *secret, uint32_t slen,
                          const uint8_t *seed,   uint32_t seedlen,
                          uint8_t *out, uint32_t olen)
{
    uint8_t A[32], tmp[32], hmac_input[32 + 256];
    uint32_t pos = 0;
    int i;
    /* A(0) = seed, A(1) = HMAC(secret, A(0)), ... */
    hmac_sha256(secret, slen, seed, seedlen, A); /* A(1) */
    while (pos < olen) {
        /* HMAC(secret, A(i) || seed) */
        if (32 + seedlen > sizeof(hmac_input)) break;
        memcpy(hmac_input, A, 32);
        memcpy(hmac_input+32, seed, seedlen);
        hmac_sha256(secret, slen, hmac_input, 32+seedlen, tmp);
        for (i = 0; i < 32 && pos < olen; i++, pos++) out[pos] = tmp[i];
        /* A(i+1) = HMAC(secret, A(i)) */
        hmac_sha256(secret, slen, A, 32, A);
    }
}

static void tls_prf(const uint8_t *secret, uint32_t slen,
                    const char *label, uint32_t llen,
                    const uint8_t *seed, uint32_t seedlen,
                    uint8_t *out, uint32_t olen)
{
    uint8_t ls[256];
    uint32_t lslen = llen + seedlen;
    if (lslen > sizeof(ls)) return;
    memcpy(ls, label, llen);
    memcpy(ls+llen, seed, seedlen);
    prf_p_sha256(secret, slen, ls, lslen, out, olen);
}

/* ---- Receive raw bytes from TCP ---- */
static int raw_recv(tls_ctx_t *ctx, uint8_t *buf, int need)
{
    int got = 0;
    while (got < need) {
        int n = tcp_recv2(ctx->fd, buf+got, need-got);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

/* ---- Receive one TLS record (pre-handshake, plaintext) ---- */
/* `cap` is the actual capacity of the caller's `data` buffer -- some
 * call sites pass stack buffers much smaller than TLS_RX_BUF (e.g.
 * an 8-byte ChangeCipherSpec buffer, a 512-byte Finished-message
 * buffer). The record-length field in `hdr` is fully controlled by
 * the remote TLS server; validating it only against the generic
 * TLS_RX_BUF (8192) instead of the real destination size let a
 * malicious/compromised server declare a length up to 8192 and have
 * raw_recv() write that many bytes into a far smaller stack array --
 * a remotely triggerable stack buffer overflow reachable from any
 * outbound HTTPS connection. */
static int recv_record(tls_ctx_t *ctx, uint8_t *type, uint8_t *data, int cap, int *len)
{
    uint8_t hdr[5];
    if (raw_recv(ctx, hdr, 5) != 0) return -1;
    *type = hdr[0];
    int rlen = u16be(hdr+3);
    if (rlen < 0 || rlen > cap) return -1;
    if (raw_recv(ctx, data, rlen) != 0) return -1;
    *len = rlen;
    return 0;
}

/* ---- Certificate verification policy ---- */
static int g_tls_verify = 1;
static int g_cert_err = 0;

void tls_set_verify(int on) { g_tls_verify = on != 0; }
int  tls_get_verify(void)   { return g_tls_verify; }
const char *tls_cert_error_detail(void) { return g_cert_err ? x509_strerror(g_cert_err) : "none"; }

/* Current UTC time for validity checks. A clock that is clearly unset (before
 * 2024) is reported as 'unknown' so the check fails closed instead of
 * accepting everything or rejecting everything arbitrarily. */
static x509_time_t tls_now(void)
{
    cmos_time_t t;
    x509_time_t r;
    cmos_get_time(&t);
    if (t.year < 2024 || t.year > 2200 || t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31) {
        r.day = INT32_MIN;
        r.sec = 0;
        return r;
    }
    return x509_time_from_ymdhms(t.year, t.month, t.day, t.hour, t.minute, t.second);
}

/* The additional authenticated data of a GCM record: the sequence number and the header
 * the record will carry, with the *plaintext* length (RFC 5288). */
static void gcm_aad(uint8_t aad[13], uint64_t seq, uint8_t type, int plain_len)
{
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)(seq >> (56 - 8 * i));
    aad[8] = type;
    aad[9] = TLS_VER_MAJOR; aad[10] = TLS_VER_MINOR;
    aad[11] = (uint8_t)(plain_len >> 8); aad[12] = (uint8_t)plain_len;
}

/* ---- AES-128-GCM record (TLS 1.2, RFC 5288) ----
 * On the wire: explicit_nonce(8) || ciphertext || tag(16). The nonce is the 4-byte salt from
 * the key block followed by those 8 explicit bytes, for which the sequence number is used --
 * it never repeats for a given key, which is what GCM requires above all else. */
static int tls_gcm_encrypt_record(tls_ctx_t *ctx, uint8_t type,
                                  const uint8_t *data, int dlen,
                                  uint8_t *out, int out_cap, int *out_len)
{
    if (dlen < 0 || dlen > 16384 || out_cap < TLS_GCM_EXPLICIT + dlen + TLS_GCM_TAG) return -1;
    uint8_t nonce[12], aad[13];
    memcpy(nonce, ctx->client_write_iv, TLS_GCM_FIXED_IV);
    for (int i = 0; i < 8; i++) nonce[TLS_GCM_FIXED_IV + i] = (uint8_t)(ctx->tx_seq >> (56 - 8 * i));
    memcpy(out, nonce + TLS_GCM_FIXED_IV, TLS_GCM_EXPLICIT);
    gcm_aad(aad, ctx->tx_seq, type, dlen);
    aes_gcm_encrypt(ctx->client_write_key, nonce, aad, sizeof(aad),
                    data, (uint32_t)dlen, out + TLS_GCM_EXPLICIT,
                    out + TLS_GCM_EXPLICIT + dlen);
    *out_len = TLS_GCM_EXPLICIT + dlen + TLS_GCM_TAG;
    ctx->tx_seq++;
    return 0;
}

static int tls_gcm_decrypt_record(tls_ctx_t *ctx, uint8_t type,
                                  const uint8_t *in, int in_len,
                                  uint8_t *out, int out_cap, int *out_len)
{
    if (in_len < TLS_GCM_EXPLICIT + TLS_GCM_TAG || in_len > TLS_REC_MAX) return -1;
    int dlen = in_len - TLS_GCM_EXPLICIT - TLS_GCM_TAG;
    if (dlen > out_cap || dlen > TLS_RX_BUF) return -1;
    uint8_t nonce[12], aad[13];
    memcpy(nonce, ctx->server_write_iv, TLS_GCM_FIXED_IV);
    memcpy(nonce + TLS_GCM_FIXED_IV, in, TLS_GCM_EXPLICIT);   /* the peer picks its own explicit half */
    gcm_aad(aad, ctx->rx_seq, type, dlen);
    if (aes_gcm_decrypt(ctx->server_write_key, nonce, aad, sizeof(aad),
                        in + TLS_GCM_EXPLICIT, (uint32_t)dlen, out,
                        in + TLS_GCM_EXPLICIT + dlen) != 0)
        return -1;
    *out_len = dlen;
    ctx->rx_seq++;
    return 0;
}

/* ---- AES-128-CBC encrypt with HMAC-SHA256 MAC (TLS 1.2 record) ---- */
static int tls_cbc_encrypt_record(tls_ctx_t *ctx, uint8_t type,
                               const uint8_t *data, int dlen,
                               uint8_t *out, int out_cap, int *out_len)
{
    /* The record must fit TLS's 16384-byte plaintext limit and the caller's
     * output buffer (IV + data + MAC + up to 16 bytes of padding); neither
     * was checked, so a long dlen overran the stack buffers below. */
    if (dlen < 0 || dlen > 16384 || out_cap < 16 + dlen + 32 + 16) return -1;
    /* MAC = HMAC-SHA256(client_mac, seq || type || version || length || data) */
    uint8_t mac_input[8+1+2+2+16384];
    int mi = 0;
    int i;
    /* seq (8 bytes big-endian) */
    for (i = 7; i >= 0; i--) { mac_input[mi++] = (uint8_t)(ctx->tx_seq >> (i*8)); }
    mac_input[mi++] = type;
    mac_input[mi++] = TLS_VER_MAJOR; mac_input[mi++] = TLS_VER_MINOR;
    mac_input[mi++] = (uint8_t)(dlen>>8); mac_input[mi++] = (uint8_t)dlen;
    memcpy(mac_input+mi, data, (uint32_t)dlen); mi += dlen;
    uint8_t mac[32];
    hmac_sha256(ctx->client_mac, 32, mac_input, (uint32_t)mi, mac);

    /* IV (random 16 bytes) */
    uint8_t iv[16];
    prng_fill(iv, 16);

    /* Plaintext = data || mac (32 bytes) */
    int pt_len = dlen + 32;
    /* PKCS#7 padding to AES block size (16) */
    int pad_len = 16 - (pt_len % 16);
    int total_pt = pt_len + pad_len;

    uint8_t plaintext[16384 + 32 + 16];
    memcpy(plaintext, data, (uint32_t)dlen);
    memcpy(plaintext+dlen, mac, 32);
    for (i = 0; i < pad_len; i++) plaintext[pt_len+i] = (uint8_t)(pad_len-1);

    /* CBC encrypt */
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    uint8_t *ct = out + 16;
    for (i = 0; i < total_pt; i += 16) {
        uint8_t blk[16];
        int j;
        for (j = 0; j < 16; j++) blk[j] = plaintext[i+j] ^ prev[j];
        aes128_encrypt(ctx->client_write_key, blk, ct+i);
        memcpy(prev, ct+i, 16);
    }
    memcpy(out, iv, 16);
    *out_len = 16 + total_pt;
    ctx->tx_seq++;
    /* the stack copies of the application data and the MAC input */
    secure_zero(plaintext, sizeof(plaintext));
    secure_zero(mac_input, sizeof(mac_input));
    secure_zero(mac, sizeof(mac));
    return 0;
}

/* ---- AES-128-CBC decrypt (TLS 1.2 record from server) ---- */
static int tls_cbc_decrypt_record(tls_ctx_t *ctx, uint8_t type,
                               const uint8_t *in, int in_len,
                               uint8_t *out, int out_cap, int *out_len)
{
    if (in_len < 16 + 48 || in_len > TLS_REC_MAX) return -1;       /* IV + at least MAC and one padding byte, block aligned */
    const uint8_t *iv = in;
    const uint8_t *ct = in + 16;
    int ct_len = in_len - 16;
    if (ct_len % 16 != 0) return -1;

    /* Scratch space on the heap: two ~16 KiB arrays on the stack came to more
     * than a 32 KiB kernel task stack can take once a record can be 16 KiB. */
    uint8_t *plaintext = (uint8_t *)malloc((size_t)ct_len);
    uint8_t *mac_input = (uint8_t *)malloc((size_t)(8 + 1 + 2 + 2 + ct_len));
    if (!plaintext || !mac_input) { free(plaintext); free(mac_input); return -1; }

    uint8_t prev[16];
    memcpy(prev, iv, 16);
    int i;
    for (i = 0; i < ct_len; i += 16) {
        aes128_decrypt(ctx->server_write_key, ct+i, plaintext+i);
        int j;
        for (j = 0; j < 16; j++) plaintext[i+j] ^= prev[j];
        memcpy(prev, ct+i, 16);
    }
    /* Remove the padding without telling the peer (or a timer) whether it was
     * the padding or the MAC that was wrong (RFC 5246 6.2.3.2): bad padding is
     * treated as 'no padding' so the MAC is still computed, and both failures
     * end in the same -1. Previously the padding bytes were not checked at all
     * (only the last one), pad values of 16..255 that the RFC allows were
     * refused, and invalid padding returned before any MAC work. */
    int rc = -1;
    uint8_t pad = plaintext[ct_len-1];
    int padlen = pad;
    int good = 1;
    if (ct_len < 32 + 1 + padlen) {
        good = 0;
        padlen = 0;
    } else {
        for (i = 0; i <= padlen; i++)                       /* padding bytes + the length byte itself */
            if (plaintext[ct_len - 1 - i] != pad) good = 0;
    }
    int data_len = ct_len - 1 - padlen - 32;
    if (data_len < 0 || data_len > out_cap || data_len > TLS_RX_BUF) goto out;   /* the caller's buffer is the limit */

    /* Verify MAC */
    int mi = 0;
    for (i = 7; i >= 0; i--) mac_input[mi++] = (uint8_t)(ctx->rx_seq >> (i*8));
    mac_input[mi++] = type;
    mac_input[mi++] = TLS_VER_MAJOR; mac_input[mi++] = TLS_VER_MINOR;
    mac_input[mi++] = (uint8_t)(data_len>>8); mac_input[mi++] = (uint8_t)data_len;
    memcpy(mac_input+mi, plaintext, (uint32_t)data_len); mi += data_len;
    uint8_t expected_mac[32];
    hmac_sha256(ctx->server_mac, 32, mac_input, (uint32_t)mi, expected_mac);
    if (!ct_equal(expected_mac, plaintext + data_len, 32) || !good) goto out;

    memcpy(out, plaintext, (uint32_t)data_len);
    *out_len = data_len;
    ctx->rx_seq++;
    rc = 0;
out:
    secure_zero(plaintext, (size_t)ct_len);
    secure_zero(mac_input, (size_t)(8 + 1 + 2 + 2 + ct_len));
    free(plaintext);
    free(mac_input);
    return rc;
}

/* Record protection, whichever suite was negotiated. */
static int tls_encrypt_record(tls_ctx_t *ctx, uint8_t type,
                               const uint8_t *data, int dlen,
                               uint8_t *out, int out_cap, int *out_len)
{
    if (suite_is_gcm(ctx)) return tls_gcm_encrypt_record(ctx, type, data, dlen, out, out_cap, out_len);
    return tls_cbc_encrypt_record(ctx, type, data, dlen, out, out_cap, out_len);
}

static int tls_decrypt_record(tls_ctx_t *ctx, uint8_t type,
                               const uint8_t *in, int in_len,
                               uint8_t *out, int out_cap, int *out_len)
{
    if (suite_is_gcm(ctx)) return tls_gcm_decrypt_record(ctx, type, in, in_len, out, out_cap, out_len);
    return tls_cbc_decrypt_record(ctx, type, in, in_len, out, out_cap, out_len);
}

/* ---- Receive encrypted TLS record, decrypt into rx_plain ---- */
static int recv_encrypted_record(tls_ctx_t *ctx)
{
    uint8_t hdr[5];
    if (raw_recv(ctx, hdr, 5) != 0) return -1;
    uint8_t type = hdr[0];
    int rlen = u16be(hdr+3);
    /* Servers send full 16 KiB records (ciphertext up to 16384 + 2048). The old
     * 8 KiB limit made every such record look like a dead connection, so large
     * HTTPS downloads came back empty. */
    if (rlen <= 0 || rlen > TLS_REC_MAX) return -1;
    uint8_t *data = (uint8_t *)malloc((size_t)rlen);
    if (!data) return -1;
    int rc = -1;
    if (raw_recv(ctx, data, rlen) != 0) goto out;

    if (type == TLS_RT_ALERT) goto out;
    if (type != TLS_RT_DATA && type != TLS_RT_HANDSHAKE) goto out;

    if (ctx->rx_plain_pos >= ctx->rx_plain_len) {
        ctx->rx_plain_len = 0; ctx->rx_plain_pos = 0;
    }
    int plain_len = 0;
    if (tls_decrypt_record(ctx, type, data, rlen, ctx->rx_plain + ctx->rx_plain_len,
                           (int)(TLS_RX_BUF - ctx->rx_plain_len), &plain_len) != 0) goto out;
    ctx->rx_plain_len += (uint32_t)plain_len;
    rc = 0;
out:
    free(data);
    return rc;
}

/* ---- Handshake diagnostics ----
 * tls_connect() used to just goto fail from any of a dozen call sites
 * and return -1, so a failed HTTPS fetch gave zero indication of
 * *where* the handshake actually broke down -- whether the server
 * rejected our only cipher suite outright (a fatal Alert record before
 * ServerHello ever arrives, likely for any server that doesn't offer
 * legacy RSA key exchange), our x509 parser couldn't find an RSA key
 * in its certificate, or the exchange completed but Finished
 * verification failed. Each step now logs to klog so dmesg shows
 * exactly how far a failed connection got. */
static void tls_log(const char *s)
{
    klog_write("tls: ");
    klog_write(s);
    klog_write("\n");
}

/* Returns 1 (and logs the alert's level/description) if `rec_type` is
 * a TLS alert record, so callers can tell "server actively refused
 * this" apart from "reply never arrived"/"garbled reply". */
static int tls_log_if_alert(uint8_t rec_type, const uint8_t *data, int len)
{
    if (rec_type != TLS_RT_ALERT) return 0;
    static const char hex[] = "0123456789abcdef";
    char buf[48];
    int i = 0;
    const char *p = "received fatal alert level=";
    while (*p) buf[i++] = *p++;
    uint8_t lvl  = len >= 1 ? data[0] : 0xFF;
    uint8_t desc = len >= 2 ? data[1] : 0xFF;
    buf[i++] = hex[(lvl >> 4) & 0xF]; buf[i++] = hex[lvl & 0xF];
    p = " desc=";
    while (*p) buf[i++] = *p++;
    buf[i++] = hex[(desc >> 4) & 0xF]; buf[i++] = hex[desc & 0xF];
    buf[i] = '\0';
    tls_log(buf);
    return 1;
}

/* ---- Handshake message reassembly ----
 * A handshake message may be split over several records and one record may
 * hold several messages. Records are appended to ctx->hs_in and complete
 * messages are handed out one at a time (and hashed into the transcript). */
#define TLS_HS_MAX_MSG   65536                 /* biggest handshake message accepted */
#define TLS_HS_IN_CAP    (4 + TLS_HS_MAX_MSG + 16384)
#define HS_ALERT         (-2)                  /* hs_next(): the server sent an alert */

static int hs_read_record(tls_ctx_t *ctx)
{
    if (ctx->hs_in_pos > 0) {                  /* drop what was already consumed */
        memmove(ctx->hs_in, ctx->hs_in + ctx->hs_in_pos, ctx->hs_in_len - ctx->hs_in_pos);
        ctx->hs_in_len -= ctx->hs_in_pos;
        ctx->hs_in_pos = 0;
    }
    uint8_t hdr[5];
    if (raw_recv(ctx, hdr, 5) != 0) return -1;
    int rlen = u16be(hdr + 3);
    if (rlen == 0 || rlen > 16384 || (uint32_t)rlen > TLS_HS_IN_CAP - ctx->hs_in_len) return -1;
    if (hdr[0] == TLS_RT_ALERT) {
        uint8_t a[2] = { 0xFF, 0xFF };
        for (int i = 0; i < rlen; i++) {      /* keep the first two bytes, discard the rest */
            uint8_t b;
            if (raw_recv(ctx, &b, 1) != 0) return -1;
            if (i < 2) a[i] = b;
        }
        tls_log_if_alert(TLS_RT_ALERT, a, 2);
        return HS_ALERT;
    }
    if (hdr[0] != TLS_RT_HANDSHAKE) return -1;
    if (raw_recv(ctx, ctx->hs_in + ctx->hs_in_len, rlen) != 0) return -1;
    ctx->hs_in_len += (uint32_t)rlen;
    return 0;
}

/* The next complete handshake message. body points into hs_in and stays valid
 * until the next call. Returns 0, HS_ALERT or -1. */
static int hs_next(tls_ctx_t *ctx, uint8_t *type, const uint8_t **body, uint32_t *blen)
{
    for (;;) {
        uint32_t avail = ctx->hs_in_len - ctx->hs_in_pos;
        if (avail >= 4) {
            uint32_t bl = u24be(ctx->hs_in + ctx->hs_in_pos + 1);
            if (bl > TLS_HS_MAX_MSG) return -1;
            if (avail - 4 >= bl) {
                *type = ctx->hs_in[ctx->hs_in_pos];
                *body = ctx->hs_in + ctx->hs_in_pos + 4;
                *blen = bl;
                hs_append(ctx, ctx->hs_in + ctx->hs_in_pos, 4 + bl);
                ctx->hs_in_pos += 4 + bl;
                return 0;
            }
        }
        int r = hs_read_record(ctx);
        if (r != 0) return r;
    }
}

/* ---- TLS handshake ---- */

/* IPv4 dotted quad or anything with a colon (IPv6). */
static int host_is_ip_literal(const char *h)
{
    int digits_dots = 1;
    for (const char *p = h; *p; p++) {
        if (*p == ':') return 1;
        if (!((*p >= '0' && *p <= '9') || *p == '.')) digits_dots = 0;
    }
    return digits_dots;
}

int tls_connect(tls_ctx_t *ctx, uint32_t ip, uint16_t port, const char *sni_host)
{
    uint8_t rec_type;
    int rc = TLS_ERR_HANDSHAKE; /* default for the fail: label; overridden at the specific sites below */

    memset(ctx, 0, sizeof(*ctx));
    sha256_init(&ctx->hs_hash);
    ctx->hs_in = (uint8_t *)malloc(TLS_HS_IN_CAP);
    if (!ctx->hs_in) return IP_ERR_NOMEM;

    ctx->fd = tcp_socket();
    if (ctx->fd < 0) { free(ctx->hs_in); ctx->hs_in = 0; return TCP_ERR_NOSOCK; }
    int trc = tcp_connect2(ctx->fd, ip, port);
    if (trc != 0) {
        tcp_close2(ctx->fd);
        free(ctx->hs_in); ctx->hs_in = 0;
        return trc; /* propagate arp_resolve()'s/IP_ERR_NOMEM/TCP_ERR_* verbatim -- see tcp.h */
    }
    tls_log("TCP connected, sending ClientHello");

    /* --- Step 1: ClientHello --- */
    /* All 32 bytes random: the old code overwrote the first four with a zero
     * "timestamp", leaving only 28 bytes of entropy in the client random. */
    prng_fill(ctx->client_rand, 32);

    uint8_t ch[512];
    int cpos = 0;
    ch[cpos++] = TLS_VER_MAJOR; ch[cpos++] = TLS_VER_MINOR; /* client version */
    memcpy(ch+cpos, ctx->client_rand, 32); cpos += 32;
    ch[cpos++] = 0; /* session ID length = 0 */
    /* cipher suites, most preferred first */
    ch[cpos++] = 0; ch[cpos++] = 6; /* 3 suites */
    ch[cpos++] = (TLS_CIPHER_ECDHE_ECDSA_AES128_GCM>>8)&0xFF;
    ch[cpos++] = TLS_CIPHER_ECDHE_ECDSA_AES128_GCM & 0xFF;
    ch[cpos++] = (TLS_CIPHER_ECDHE_RSA_AES128_GCM>>8)&0xFF;
    ch[cpos++] = TLS_CIPHER_ECDHE_RSA_AES128_GCM & 0xFF;
    ch[cpos++] = (TLS_CIPHER_RSA_AES128_CBC_SHA256>>8)&0xFF;
    ch[cpos++] = TLS_CIPHER_RSA_AES128_CBC_SHA256 & 0xFF;
    /* compression: null only */
    ch[cpos++] = 1; ch[cpos++] = 0;

    /* extensions: the ClientHello's one "extensions" block can hold
     * several -- a single 2-byte length up front covers all of them
     * combined, then each is its own type+length+data. */
    int name_len = sni_host ? (int)strlen(sni_host) : 0;
    /* RFC 6066 3: the server_name extension carries DNS host names only, never
     * a literal IP address. */
    int have_sni = (name_len > 0 && name_len < 250 && !host_is_ip_literal(sni_host));
    {
        int all_ext_pos = cpos;
        cpos += 2; /* combined extensions length, filled in below */

        if (have_sni) {
            /* server_name (SNI, RFC 6066) -- without this, a proxy/CDN
             * fronting many hostnames on one IP (e.g. Cloudflare) has
             * no way to know which origin/certificate this connection
             * is for. */
            ch[cpos++] = 0x00; ch[cpos++] = 0x00; /* extension type: server_name */
            ch[cpos++] = 0x00; ch[cpos++] = (uint8_t)(name_len + 5); /* extension_data length */
            ch[cpos++] = 0x00; ch[cpos++] = (uint8_t)(name_len + 3); /* server_name_list length */
            ch[cpos++] = 0x00; /* name_type: host_name */
            ch[cpos++] = 0x00; ch[cpos++] = (uint8_t)name_len; /* HostName length */
            memcpy(ch + cpos, sni_host, (size_t)name_len); cpos += name_len;
        }

        /* signature_algorithms (RFC 5246 7.4.1.4.1) -- mandatory in
         * practice for a TLS 1.2 RSA handshake against any modern
         * stack (OpenSSL etc): without it, a server can't tell we
         * support SHA-256 signatures and commonly just fails the
         * handshake rather than guess. Advertise the one algorithm
         * this client actually verifies against: rsa_pkcs1_sha256. */
        ch[cpos++] = 0x00; ch[cpos++] = 0x0d; /* extension type: signature_algorithms */
        ch[cpos++] = 0x00; ch[cpos++] = 0x0E; /* extension_data length */
        ch[cpos++] = 0x00; ch[cpos++] = 0x0C; /* supported_signature_algorithms length */
        ch[cpos++] = 0x04; ch[cpos++] = 0x03; /* sha256, ecdsa (P-256 certificates) */
        ch[cpos++] = 0x05; ch[cpos++] = 0x03; /* sha384, ecdsa */
        ch[cpos++] = 0x06; ch[cpos++] = 0x03; /* sha512, ecdsa */
        ch[cpos++] = 0x04; ch[cpos++] = 0x01; /* sha256, rsa */
        ch[cpos++] = 0x05; ch[cpos++] = 0x01; /* sha384, rsa */
        ch[cpos++] = 0x06; ch[cpos++] = 0x01; /* sha512, rsa -- the server signs its key exchange
                                               * with one of these, and all six are verified here */

        /* supported_groups (RFC 8422 5.1.1): which curves we can do an ephemeral exchange on.
         * X25519 is the one implemented, and the one modern servers prefer anyway. */
        ch[cpos++] = 0x00; ch[cpos++] = 0x0a; /* extension type: supported_groups */
        ch[cpos++] = 0x00; ch[cpos++] = 0x06; /* extension_data length */
        ch[cpos++] = 0x00; ch[cpos++] = 0x04; /* list length */
        ch[cpos++] = (TLS_GROUP_X25519 >> 8) & 0xFF; ch[cpos++] = TLS_GROUP_X25519 & 0xFF;
        /* secp256r1 is listed as well because RFC 4492's curve list also says which curves a
         * *certificate* key may use: without it a server with an ECDSA certificate decides it
         * has nothing it can offer us. X25519 stays first, so that is what the key exchange uses. */
        ch[cpos++] = (TLS_GROUP_SECP256R1 >> 8) & 0xFF; ch[cpos++] = TLS_GROUP_SECP256R1 & 0xFF;

        /* ec_point_formats: uncompressed only. X25519 has no point format of its own, but
         * servers still expect the extension when a curve is offered. */
        ch[cpos++] = 0x00; ch[cpos++] = 0x0b; /* extension type: ec_point_formats */
        ch[cpos++] = 0x00; ch[cpos++] = 0x02; /* extension_data length */
        ch[cpos++] = 0x01; ch[cpos++] = 0x00; /* one format: uncompressed */

        int all_ext_total = cpos - (all_ext_pos + 2);
        ch[all_ext_pos] = (uint8_t)((all_ext_total >> 8) & 0xFF);
        ch[all_ext_pos + 1] = (uint8_t)(all_ext_total & 0xFF);
    }

    if (tls_send_hs(ctx, TLS_HT_CLIENT_HELLO, ch, cpos) != 0) {
        tls_log("ClientHello send failed");
        goto fail;
    }

    /* --- Step 2: ServerHello --- */
    {
        uint8_t ht; const uint8_t *hbody; uint32_t hlen;
        int hr = hs_next(ctx, &ht, &hbody, &hlen);
        if (hr == HS_ALERT) { rc = TLS_ERR_ALERT; goto fail; }
        if (hr != 0) {
            tls_log("no reply after ClientHello (connection closed or timed out)");
            goto fail;
        }
        if (ht != TLS_HT_SERVER_HELLO) {
            tls_log("expected ServerHello, got a different handshake message");
            goto fail;
        }
        /* version(2) random(32) session_id_len(1) session_id suite(2) compression(1)
         * [extensions_len(2) extensions]. Every length is checked against the
         * message, and the parameters must be what we offered: before, a
         * different suite or version was only 'tolerated' and the handshake
         * failed later (or worse, ran on mismatched keys). */
        if (hlen < 38) {
            tls_log("ServerHello message too short to parse");
            goto fail;
        }
        if (hbody[0] != TLS_VER_MAJOR || hbody[1] != TLS_VER_MINOR) {
            tls_log("server did not negotiate TLS 1.2");
            goto fail;
        }
        memcpy(ctx->server_rand, hbody+2, 32);
        uint32_t sid_len = hbody[34];
        if (sid_len > 32 || 35 + sid_len + 3 > hlen) {
            tls_log("ServerHello session id / parameters exceed the message");
            goto fail;
        }
        {
            uint16_t chosen = u16be(hbody + 35 + sid_len);
            if (chosen != TLS_CIPHER_ECDHE_ECDSA_AES128_GCM &&
                chosen != TLS_CIPHER_ECDHE_RSA_AES128_GCM && chosen != TLS_CIPHER_RSA_AES128_CBC_SHA256) {
                tls_log("server chose a cipher suite we did not offer");
                goto fail;
            }
            ctx->suite = chosen;
            tls_log(chosen == TLS_CIPHER_ECDHE_ECDSA_AES128_GCM ? "suite: ECDHE-ECDSA-AES128-GCM-SHA256"
                    : chosen == TLS_CIPHER_ECDHE_RSA_AES128_GCM  ? "suite: ECDHE-RSA-AES128-GCM-SHA256"
                                                                 : "suite: RSA-AES128-CBC-SHA256");
        }
        if (hbody[37 + sid_len] != 0) {
            tls_log("server chose a compression method we did not offer");
            goto fail;
        }
        {
            uint32_t rest = hlen - (38 + sid_len);
            if (rest != 0 && (rest < 2 || (uint32_t)u16be(hbody + 38 + sid_len) != rest - 2)) {
                tls_log("ServerHello extensions length is inconsistent");
                goto fail;
            }
        }
        tls_log("ServerHello received");
    }

    /* --- Step 3: Certificate ... ServerHelloDone (may span several records) --- */
    uint8_t rsa_mod[RSA_MAX_BYTES];
    int rsa_mod_len = 0;
    uint32_t rsa_exp = 65537;
    uint8_t ec_pub[65];
    int found_key = 0;
    int got_cert = 0;
    int got_done = 0;
    int got_ske = 0;
    uint8_t server_pub[X25519_LEN];
    while (!got_done) {
        uint8_t ht; const uint8_t *hbody; uint32_t hlen;
        int hr = hs_next(ctx, &ht, &hbody, &hlen);
        if (hr == HS_ALERT) { rc = TLS_ERR_ALERT; goto fail; }
        if (hr != 0) {
            tls_log("no reply while waiting for Certificate/ServerHelloDone");
            goto fail;
        }
        if (ht == TLS_HT_CERTIFICATE) {
            if (got_cert) { tls_log("duplicate Certificate message"); goto fail; }
            got_cert = 1;
            /* certificates_list: 3-byte total length, then each cert 3-byte length + DER */
            if (hlen < 3 || u24be(hbody) != hlen - 3) {
                tls_log("Certificate list length does not match the message");
                goto fail;
            }
            x509_der_t chain[X509_MAX_CHAIN];
            int nc = 0;
            uint32_t cp = 3;
            while (cp < hlen) {
                if (hlen - cp < 3) { tls_log("truncated certificate entry"); goto fail; }
                uint32_t clen = u24be(hbody+cp); cp += 3;
                if (clen == 0 || clen > hlen - cp) {    /* empty, or runs past the message */
                    tls_log("certificate length exceeds the Certificate message");
                    goto fail;
                }
                if (nc >= X509_MAX_CHAIN) {
                    if (g_tls_verify) {                 /* a validated chain is bounded */
                        g_cert_err = X509_ERR_CHAIN_TOO_LONG;
                        tls_log(x509_strerror(g_cert_err));
                        rc = TLS_ERR_CERT;
                        goto fail;
                    }
                    cp += clen;                         /* unverified: only the leaf matters */
                    continue;
                }
                chain[nc].der = hbody + cp;
                chain[nc].len = clen;
                nc++;
                cp += clen;
            }
            if (nc == 0) { tls_log("server sent an empty certificate list"); goto fail; }

            /* Authenticate the server: chain to a trusted root, valid now, and
             * issued for the host we meant to reach. The leaf must also allow
             * RSA key transport. (When verification was switched off the leaf
             * is still parsed, but nothing vouches for it.) */
            x509_cert_t leaf;
            int xr;
            /* What the server's key is used for depends on the suite: RSA key transport
             * encrypts to it, an ECDHE handshake only signs with it. */
            int ku_flag = suite_is_gcm(ctx) ? X509_F_SIGNATURE : X509_F_RSA_KEY_EXCHANGE;
            if (g_tls_verify)
                xr = x509_verify_chain(chain, nc, sni_host, tls_now(), ku_flag, &leaf);
            else
                xr = x509_parse(chain[0].der, chain[0].len, &leaf);
            /* The suite says which kind of key the server must have signed with. */
            if (xr == X509_OK) {
                if (suite_is_ecdsa(ctx)) { if (!leaf.has_ec_key) xr = X509_ERR_UNSUPPORTED_KEY; }
                else if (!leaf.has_rsa_key) xr = X509_ERR_UNSUPPORTED_KEY;
            }
            if (xr != X509_OK) {
                g_cert_err = xr;
                tls_log(x509_strerror(xr));
                rc = TLS_ERR_CERT;
                goto fail;
            }
            if (leaf.has_rsa_key) {
                memcpy(rsa_mod, leaf.n, leaf.n_len);
                rsa_mod_len = (int)leaf.n_len;
                rsa_exp = leaf.e;
            }
            if (leaf.has_ec_key) memcpy(ec_pub, leaf.ec_point, sizeof(ec_pub));
            found_key = 1;
        } else if (ht == TLS_HT_SERVER_DONE) {
            if (!got_cert) { tls_log("ServerHelloDone before the Certificate"); goto fail; }
            if (hlen != 0) { tls_log("ServerHelloDone is not empty"); goto fail; }
            got_done = 1;
        } else if (ht == TLS_HT_SERVER_KEY_EX) {
            if (!suite_is_gcm(ctx)) {
                tls_log("ServerKeyExchange in a plain-RSA handshake");
                goto fail;
            }
            if (!found_key) { tls_log("ServerKeyExchange before the Certificate"); goto fail; }
            if (got_ske) { tls_log("duplicate ServerKeyExchange"); goto fail; }
            /* ServerECDHParams: curve_type(1) named_curve(2) point_len(1) point,
             * then the signature over it: algorithm(2) length(2) signature. */
            if (hlen < 4) { tls_log("ServerKeyExchange too short"); goto fail; }
            if (hbody[0] != 3 || u16be(hbody + 1) != TLS_GROUP_X25519) {
                tls_log("server picked a curve we did not offer");
                goto fail;
            }
            uint32_t plen = hbody[3];
            if (plen != X25519_LEN || hlen < 4 + plen + 4) {
                tls_log("ServerKeyExchange public key is not a 32-byte X25519 point");
                goto fail;
            }
            uint32_t params_len = 4 + plen;             /* exactly what the signature covers */
            const uint8_t *sig_alg = hbody + params_len;
            uint32_t sig_len = u16be(sig_alg + 2);
            const uint8_t *sig = sig_alg + 4;
            if (params_len + 4 + sig_len != hlen) {
                tls_log("ServerKeyExchange signature length does not match the message");
                goto fail;
            }
            /* signature algorithm byte: 1 = RSA (PKCS#1 v1.5), 3 = ECDSA */
            if ((sig_alg[1] != 1 && sig_alg[1] != 3) ||
                (sig_alg[1] == 3) != (suite_is_ecdsa(ctx) != 0)) {
                tls_log("server signed its key exchange with an algorithm we cannot verify");
                goto fail;
            }
            /* The signature is over client_random || server_random || the params above, which
             * is what binds this ephemeral key to the certificate we just authenticated --
             * without checking it, anyone in the path could substitute their own key. */
            int hash_id, digest_len;
            uint8_t digest[64];
            uint8_t signed_data[32 + 32 + 4 + X25519_LEN];
            memcpy(signed_data, ctx->client_rand, 32);
            memcpy(signed_data + 32, ctx->server_rand, 32);
            memcpy(signed_data + 64, hbody, params_len);
            uint32_t signed_len = 64 + params_len;
            if (sig_alg[0] == 4)      { hash_id = RSA_HASH_SHA256; digest_len = 32; sha256_hash(signed_data, signed_len, digest); }
            else if (sig_alg[0] == 5) { hash_id = RSA_HASH_SHA384; digest_len = 48; sha384_hash(signed_data, signed_len, digest); }
            else if (sig_alg[0] == 6) { hash_id = RSA_HASH_SHA512; digest_len = 64; sha512_hash(signed_data, signed_len, digest); }
            else { tls_log("server used a hash we did not offer for its key exchange"); goto fail; }
            int sig_ok;
            if (sig_alg[1] == 3) {
                uint8_t sr[32], ss[32];
                sig_ok = ecdsa_sig_to_rs(sig, sig_len, sr, ss) == 0 &&
                         p256_ecdsa_verify(ec_pub, sr, ss, digest, digest_len) == 0;
            } else {
                sig_ok = rsa_pkcs1_verify(rsa_mod, rsa_mod_len, rsa_exp, hash_id, digest, sig, (int)sig_len) == 0;
            }
            if (!sig_ok) {
                tls_log("ServerKeyExchange signature is not valid for this certificate");
                goto fail;
            }
            memcpy(server_pub, hbody + 4, X25519_LEN);
            got_ske = 1;
            tls_log("ServerKeyExchange verified");
        } else if (ht == TLS_HT_CERT_REQUEST) {
            tls_log("server requests a client certificate (not supported)");
            goto fail;
        } else {
            tls_log("unexpected handshake message while waiting for ServerHelloDone");
            goto fail;
        }
    }
    if (!found_key) {
        tls_log("no usable RSA key in the server certificate");
        goto fail;
    }
    tls_log(g_tls_verify ? "certificate chain verified" : "certificate accepted WITHOUT verification");
    if (ctx->hs_in_pos != ctx->hs_in_len) {
        tls_log("unexpected data after ServerHelloDone");
        goto fail;
    }

    /* --- Step 4: ClientKeyExchange --- */
    uint8_t premaster[48];
    uint32_t premaster_len;
    if (suite_is_gcm(ctx)) {
        if (!got_ske) { tls_log("ECDHE handshake without a ServerKeyExchange"); goto fail; }
        /* An ephemeral key pair used once and then forgotten: this is where forward secrecy
         * comes from, and the scalar never leaves this function. */
        uint8_t scalar[X25519_LEN], our_pub[X25519_LEN];
        prng_fill(scalar, X25519_LEN);
        x25519_base(our_pub, scalar);
        if (x25519(premaster, scalar, server_pub) != 0) {
            secure_zero(scalar, sizeof(scalar));
            tls_log("server sent a degenerate X25519 point");
            goto fail;
        }
        secure_zero(scalar, sizeof(scalar));
        premaster_len = X25519_LEN;

        uint8_t cke[1 + X25519_LEN];
        cke[0] = X25519_LEN;
        memcpy(cke + 1, our_pub, X25519_LEN);
        if (tls_send_hs(ctx, TLS_HT_CLIENT_KEY_EX, cke, sizeof(cke)) != 0) {
            tls_log("ClientKeyExchange send failed");
            goto fail;
        }
    } else {
        premaster[0] = TLS_VER_MAJOR; premaster[1] = TLS_VER_MINOR;
        prng_fill(premaster+2, 46);
        premaster_len = 48;

        uint8_t enc_pm[RSA_MAX_BYTES];
        if (rsa_pkcs1_encrypt(rsa_mod, rsa_mod_len, rsa_exp, premaster, 48, enc_pm) != 0) {
            tls_log("RSA encrypt of premaster secret failed");
            goto fail;
        }

        uint8_t cke[2 + RSA_MAX_BYTES];
        cke[0] = (uint8_t)(rsa_mod_len >> 8);
        cke[1] = (uint8_t)rsa_mod_len;
        memcpy(cke+2, enc_pm, (size_t)rsa_mod_len);
        if (tls_send_hs(ctx, TLS_HT_CLIENT_KEY_EX, cke, 2 + rsa_mod_len) != 0) {
            tls_log("ClientKeyExchange send failed");
            goto fail;
        }
    }
    tls_log("ClientKeyExchange sent");

    /* --- Step 5: Compute master secret --- */
    {
        uint8_t seed[64];
        memcpy(seed, ctx->client_rand, 32);
        memcpy(seed+32, ctx->server_rand, 32);
        tls_prf(premaster, premaster_len, "master secret", 13, seed, 64, ctx->master, 48);
    }
    secure_zero(premaster, sizeof(premaster));          /* no longer needed */

    /* --- Step 6: Derive key material --- */
    {
        uint8_t seed[64];
        memcpy(seed, ctx->server_rand, 32);
        memcpy(seed+32, ctx->client_rand, 32);
        uint8_t km[128];
        if (suite_is_gcm(ctx)) {
            /* GCM authenticates with the cipher itself, so there are no MAC keys: the block is
             * two 16-byte keys and two 4-byte nonce salts (RFC 5288). */
            tls_prf(ctx->master, 48, "key expansion", 13, seed, 64, km, 40);
            memcpy(ctx->client_write_key, km,      16);
            memcpy(ctx->server_write_key, km + 16, 16);
            memcpy(ctx->client_write_iv,  km + 32,  TLS_GCM_FIXED_IV);
            memcpy(ctx->server_write_iv,  km + 36,  TLS_GCM_FIXED_IV);
        } else {
            /* 2*32 (MAC) + 2*16 (key) + 2*16 (IV) = 128 bytes */
            tls_prf(ctx->master, 48, "key expansion", 13, seed, 64, km, 128);
            memcpy(ctx->client_mac,      km,    32);
            memcpy(ctx->server_mac,      km+32, 32);
            memcpy(ctx->client_write_key,km+64, 16);
            memcpy(ctx->server_write_key,km+80, 16);
            memcpy(ctx->client_write_iv, km+96, 16);
            memcpy(ctx->server_write_iv, km+112,16);
        }
        secure_zero(km, sizeof(km));
    }

    /* --- Step 7: ChangeCipherSpec --- */
    {
        uint8_t ccs = 1;
        if (tls_send_raw(ctx, TLS_RT_CHANGE_CIPHER, &ccs, 1) != 0) {
            tls_log("ChangeCipherSpec send failed");
            goto fail;
        }
    }

    /* --- Step 8: Finished --- */
    {
        /* verify_data = PRF(master, "client finished", SHA256(all_handshake_messages))[0..11] */
        uint8_t hs_hash[32];
        hs_hash_now(ctx, hs_hash);
        uint8_t verify[12];
        tls_prf(ctx->master, 48, "client finished", 15, hs_hash, 32, verify, 12);

        /* Finished message body */
        uint8_t fin_body[12];
        memcpy(fin_body, verify, 12);

        /* Build handshake message to add to transcript */
        uint8_t fin_hs[16];
        fin_hs[0] = TLS_HT_FINISHED;
        put_u24(fin_hs+1, 12);
        memcpy(fin_hs+4, fin_body, 12);
        hs_append(ctx, fin_hs, 16);

        /* Encrypt and send */
        uint8_t enc[512];
        int enc_len = 0;
        if (tls_encrypt_record(ctx, TLS_RT_HANDSHAKE, fin_hs, 16, enc, (int)sizeof(enc), &enc_len) != 0) {
            tls_log("encrypting client Finished failed");
            goto fail;
        }
        if (tls_send_raw(ctx, TLS_RT_HANDSHAKE, enc, enc_len) != 0) {
            tls_log("client Finished send failed");
            goto fail;
        }
        tls_log("client Finished sent, waiting for server ChangeCipherSpec+Finished");
    }

    /* --- Step 9: Receive server ChangeCipherSpec + Finished --- */
    tcp_set_debug_trace(1);
    {
        /* ChangeCipherSpec */
        uint8_t ccs_data[8];
        int ccs_len;
        if (recv_record(ctx, &rec_type, ccs_data, (int)sizeof(ccs_data), &ccs_len) != 0) {
            tls_log("no reply after client Finished (connection closed or timed out)");
            goto fail;
        }
        if (tls_log_if_alert(rec_type, ccs_data, ccs_len)) { rc = TLS_ERR_ALERT; goto fail; }
        if (rec_type != TLS_RT_CHANGE_CIPHER) {
            tls_log("expected server ChangeCipherSpec, got a different record type");
            goto fail;
        }
        if (ccs_len != 1 || ccs_data[0] != 1) {
            tls_log("malformed server ChangeCipherSpec");
            goto fail;
        }

        /* Encrypted Finished */
        uint8_t ef_data[512];
        int ef_len;
        if (recv_record(ctx, &rec_type, ef_data, (int)sizeof(ef_data), &ef_len) != 0) {
            tls_log("no reply after server ChangeCipherSpec (connection closed or timed out)");
            goto fail;
        }
        if (rec_type != TLS_RT_HANDSHAKE) {
            tls_log("expected encrypted server Finished, got a different record type");
            goto fail;
        }

        uint8_t fin_plain[256];
        int fin_plen = 0;
        if (tls_decrypt_record(ctx, TLS_RT_HANDSHAKE, ef_data, ef_len, fin_plain, (int)sizeof(fin_plain), &fin_plen) != 0) {
            tls_log("decrypting server Finished failed");
            goto fail;
        }

        /* Verify server Finished: exactly one 12-byte Finished message */
        if (fin_plen != 16 || fin_plain[0] != TLS_HT_FINISHED || u24be(fin_plain + 1) != 12) {
            tls_log("server Finished message is malformed");
            goto fail;
        }
        uint8_t hs_hash[32];
        hs_hash_now(ctx, hs_hash);
        uint8_t expected[12];
        tls_prf(ctx->master, 48, "server finished", 15, hs_hash, 32, expected, 12);
        if (!ct_equal(fin_plain+4, expected, 12)) {
            tls_log("server Finished verification failed (MAC/hash mismatch)");
            goto fail;
        }
    }

    tls_log("handshake complete");
    free(ctx->hs_in); ctx->hs_in = 0;
    ctx->handshake_done = 1;
    tcp_set_debug_trace(0);
    return 0;
fail:
    tcp_set_debug_trace(0);
    tcp_close2(ctx->fd);
    ctx->fd = -1;
    free(ctx->hs_in); ctx->hs_in = 0;
    secure_zero(premaster, sizeof(premaster));
    tls_wipe_keys(ctx);
    return rc;
}

const char *tls_connect_strerror(int err)
{
    switch (err) {
        case TLS_ERR_ALERT:     return "server sent a fatal TLS alert (see dmesg for level/description)";
        case TLS_ERR_HANDSHAKE: return "TLS handshake failed (see dmesg for which step)";
        case TLS_ERR_CERT: {
            static char msg[128];
            const char *p = "server certificate rejected: ";
            int i = 0;
            while (*p && i < 100) msg[i++] = *p++;
            p = tls_cert_error_detail();
            while (*p && i < 126) msg[i++] = *p++;
            msg[i] = 0;
            return msg;
        }
        case HTTP_ERR_REQUEST:
        case HTTP_ERR_HEADER:   return http_strerror(err);   /* https_get() shares http.c's request/response checks */
        case TCP_ERR_NOSOCK:    return "no free TCP socket";
        case TCP_ERR_REFUSED:   return "connection refused (RST received)";
        case TCP_ERR_TIMEOUT:   return "connection timed out, no reply to SYN";
        case IP_ERR_NOMEM:      return "out of memory building packet";
        default:                 return arp_resolve_strerror(err); /* ARP_ERR_* -- couldn't even send the SYN */
    }
}

int tls_write(tls_ctx_t *ctx, const uint8_t *data, int len)
{
    /* The whole buffer used to go into one record encrypted into a 4352-byte
     * stack array, so any send above ~4 KB (Python's socket.send() over TLS,
     * for one) smashed the stack. Send it as records of at most TLS_TX_BUF
     * bytes instead. */
    if (!ctx || ctx->fd < 0 || !ctx->handshake_done || len < 0) return -1;
    int off = 0;
    while (off < len) {
        int n = len - off < TLS_TX_BUF ? len - off : TLS_TX_BUF;
        uint8_t enc[TLS_TX_BUF + 256];
        int enc_len = 0;
        if (tls_encrypt_record(ctx, TLS_RT_DATA, data + off, n, enc, (int)sizeof(enc), &enc_len) != 0) return -1;
        if (tls_send_raw(ctx, TLS_RT_DATA, enc, enc_len) != 0) return -1;
        off += n;
    }
    return 0;
}

int tls_read(tls_ctx_t *ctx, uint8_t *buf, int max)
{
    /* Return from buffer first */
    if (ctx->rx_plain_pos < ctx->rx_plain_len) {
        int avail = (int)(ctx->rx_plain_len - ctx->rx_plain_pos);
        int n = avail < max ? avail : max;
        memcpy(buf, ctx->rx_plain + ctx->rx_plain_pos, (uint32_t)n);
        ctx->rx_plain_pos += (uint32_t)n;
        return n;
    }
    /* Receive a new record */
    ctx->rx_plain_len = 0; ctx->rx_plain_pos = 0;
    if (recv_encrypted_record(ctx) != 0) return 0;
    int avail = (int)ctx->rx_plain_len;
    int n = avail < max ? avail : max;
    memcpy(buf, ctx->rx_plain, (uint32_t)n);
    ctx->rx_plain_pos = (uint32_t)n;
    return n;
}

void tls_close(tls_ctx_t *ctx)
{
    if (ctx->fd >= 0) {
        /* Send close_notify alert */
        uint8_t alert[2] = {1, 0};
        uint8_t enc[256]; int enc_len = 0;
        tls_encrypt_record(ctx, TLS_RT_ALERT, alert, 2, enc, (int)sizeof(enc), &enc_len);
        tls_send_raw(ctx, TLS_RT_ALERT, enc, enc_len);
        tcp_close2(ctx->fd);
        ctx->fd = -1;
    }
    /* the session is over: do not leave its keys and the last plaintext in memory */
    tls_wipe_keys(ctx);
    ctx->handshake_done = 0;
}

/* Suppress unused warning for u32be */
static uint32_t __attribute__((unused)) _u32 = 0;
static void __attribute__((unused)) _unused_u32be(void) { _u32 = u32be((uint8_t*)&_u32); }
static void __attribute__((unused)) _unused_put_u8(void) { uint8_t x; put_u8(&x,0); }
