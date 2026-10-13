/* Host test: ECDSA/P-256 verification against known-good signatures from t_p256.py. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "p256.h"
static int unhex(const char *h, uint8_t *o) { int n=0;
    while (h[2*n] && h[2*n+1]) { int hi=h[2*n]<='9'?h[2*n]-'0':(h[2*n]|32)-'a'+10,
        lo=h[2*n+1]<='9'?h[2*n+1]-'0':(h[2*n+1]|32)-'a'+10; o[n]=(uint8_t)((hi<<4)|lo); n++; } return n; }
int main(int argc, char **argv) {
    /* argv: pub r s digest expect(0|1) ... repeated */
    int bad = 0, n = 0;
    for (int i = 1; i + 4 < argc; i += 5) {
        uint8_t pub[65], r[32], s[32], dg[64];
        unhex(argv[i], pub); unhex(argv[i+1], r); unhex(argv[i+2], s);
        int dl = unhex(argv[i+3], dg);
        int want = argv[i+4][0] == '1';
        int got = p256_ecdsa_verify(pub, r, s, dg, dl) == 0;
        if (got != want) { printf("case %d: got %s, want %s\n", n, got?"valid":"invalid", want?"valid":"invalid"); bad++; }
        n++;
    }
    printf("ECDSA P-256: %d cases, %s\n", n, bad ? "FAILED" : "ok");
    return bad != 0;
}
