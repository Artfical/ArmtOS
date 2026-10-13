#ifndef CHACHA20_H
#define CHACHA20_H

#include <stdint.h>

/* ChaCha20 stream cipher (RFC 7539) */
void chacha20_block(const uint8_t key[32], uint32_t counter,
                    const uint8_t nonce[12], uint8_t out[64]);

void chacha20_encrypt(const uint8_t key[32], uint32_t counter,
                      const uint8_t nonce[12],
                      const uint8_t *in, uint8_t *out, int len);

/* HChaCha20 — used by XChaCha20 to derive sub-key from 256-bit nonce */
void hchacha20(const uint8_t key[32], const uint8_t nonce16[16],
               uint8_t out[32]);

/* XChaCha20 — 192-bit nonce variant */
void xchacha20_encrypt(const uint8_t key[32], const uint8_t nonce[24],
                       uint32_t counter,
                       const uint8_t *in, uint8_t *out, int len);

/* Poly1305 MAC (RFC 8439) */
void poly1305_mac(const uint8_t key[32],
                  const uint8_t *msg, int msg_len,
                  uint8_t tag[16]);

/*
 * XChaCha20-Poly1305 AEAD (simplified — no AAD for now).
 * Nonce: 24 bytes, key: 32 bytes.
 * Encrypt: out = ciphertext || tag (len+16 bytes written).
 * Decrypt: returns 0 on success, -1 on MAC failure.
 */
void xchacha20poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[24],
                               const uint8_t *plain, int plain_len,
                               uint8_t *out);

int xchacha20poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[24],
                              const uint8_t *cipher, int cipher_len,
                              uint8_t *plain);

/*
 * ChaCha20-Poly1305 AEAD exactly as RFC 8439 defines it: a 12-byte nonce and additional
 * authenticated data. This is the cipher most large sites prefer over AES-GCM, because it
 * is fast without the AES instructions that older or smaller machines lack.
 */
void chacha20poly1305_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                              const uint8_t *aad, int aad_len,
                              const uint8_t *plain, int plain_len,
                              uint8_t *cipher, uint8_t tag[16]);

/* 0 = the tag matched; -1 = forged or corrupt, and `plain` is cleared. */
int  chacha20poly1305_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                              const uint8_t *aad, int aad_len,
                              const uint8_t *cipher, int cipher_len,
                              uint8_t *plain, const uint8_t tag[16]);

#endif
