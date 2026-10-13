/* Prints randomised ChaCha20-Poly1305 AEAD cases for t_cc20.py to check against a
   known-good implementation, and checks the RFC 8439 section 2.8.2 vector. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "chacha20.h"
static void phex(const uint8_t *b, int n) { for (int i=0;i<n;i++) printf("%02x", b[i]); }
static int unhex(const char *h, uint8_t *o) { int n=0;
    while (h[2*n] && h[2*n+1]) { int hi=h[2*n]<='9'?h[2*n]-'0':(h[2*n]|32)-'a'+10,
        lo=h[2*n+1]<='9'?h[2*n+1]-'0':(h[2*n+1]|32)-'a'+10; o[n]=(uint8_t)((hi<<4)|lo); n++; } return n; }
int main(void) {
    /* RFC 8439 2.8.2 */
    uint8_t k[32], n[12], aad[12], tag[16], ct[200], pt[200];
    unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", k);
    unhex("070000004041424344454647", n);
    unhex("50515253c0c1c2c3c4c5c6c7", aad);
    const char *msg = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    int ml = (int)strlen(msg);
    chacha20poly1305_encrypt(k, n, aad, 12, (const uint8_t *)msg, ml, ct, tag);
    printf("rfc_ct "); phex(ct, ml); printf("\n");
    printf("rfc_tag "); phex(tag, 16); printf("\n");
    if (chacha20poly1305_decrypt(k, n, aad, 12, ct, ml, pt, tag) != 0 || memcmp(pt, msg, (size_t)ml))
        printf("rfc_roundtrip FAILED\n");
    uint8_t bad[16]; memcpy(bad, tag, 16); bad[0] ^= 1;
    if (chacha20poly1305_decrypt(k, n, aad, 12, ct, ml, pt, bad) == 0) printf("forged tag ACCEPTED\n");
    /* randomised cases */
    uint32_t s = 999;
    for (int c = 0; c < 40; c++) {
        uint8_t kk[32], nn[12], aa[48], pp[160], cc[160], tt[16];
        int al = c % 30, pl = (c * 11) % 160;
        for (int i=0;i<32;i++){ s=s*1103515245+12345; kk[i]=(uint8_t)(s>>16); }
        for (int i=0;i<12;i++){ s=s*1103515245+12345; nn[i]=(uint8_t)(s>>16); }
        for (int i=0;i<al;i++){ s=s*1103515245+12345; aa[i]=(uint8_t)(s>>16); }
        for (int i=0;i<pl;i++){ s=s*1103515245+12345; pp[i]=(uint8_t)(s>>16); }
        chacha20poly1305_encrypt(kk, nn, aa, al, pp, pl, cc, tt);
        printf("case "); phex(kk,32); printf(" "); phex(nn,12); printf(" ");
        if (al) phex(aa,al); else printf("-"); printf(" ");
        if (pl) phex(pp,pl); else printf("-"); printf(" ");
        if (pl) phex(cc,pl); else printf("-"); printf(" "); phex(tt,16); printf("\n");
    }
    return 0;
}
