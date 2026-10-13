/* Host test: ECDSA/P-256 verification against known-good signatures from t_p256.py. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "p256.h"
static int unhex(const char *h, uint8_t *o) { int n=0;
    while (h[2*n] && h[2*n+1]) { int hi=h[2*n]<='9'?h[2*n]-'0':(h[2*n]|32)-'a'+10,
        lo=h[2*n+1]<='9'?h[2*n+1]-'0':(h[2*n+1]|32)-'a'+10; o[n]=(uint8_t)((hi<<4)|lo); n++; } return n; }
int main(int argc, char **argv) {
    /* argv: curve pub r s digest expect(0|1) ... repeated */
    int bad = 0, n = 0;
    for (int i = 1; i + 5 < argc; i += 6) {
        uint8_t pub[97], r[48], s[48], dg[64];
        int curve = argv[i][0] == '3' ? EC_CURVE_P384 : EC_CURVE_P256;
        int pl = unhex(argv[i+1], pub); unhex(argv[i+2], r); unhex(argv[i+3], s);
        int dl = unhex(argv[i+4], dg);
        int want = argv[i+5][0] == '1';
        int got = ec_ecdsa_verify(curve, pub, (uint32_t)pl, r, s, dg, dl) == 0;
        if (got != want) { printf("case %d: got %s, want %s\n", n, got?"valid":"invalid", want?"valid":"invalid"); bad++; }
        n++;
    }
    printf("ECDSA P-256/P-384: %d cases, %s\n", n, bad ? "FAILED" : "ok");
    return bad != 0;
}
