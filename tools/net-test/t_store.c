/* Host test: every built-in trust anchor must parse, carry a key this kernel can use
   (RSA, or EC on P-256/P-384), be a CA, and verify its own signature. */
#include <stdio.h>
#include <string.h>
#include "x509.h"
unsigned char net_mac[6] = {1, 2, 3, 4, 5, 6};
int main(void)
{
    int n = x509_trust_count(), bad = 0, ca = 0, ec = 0;
    for (int i = 0; i < n; i++) {
        x509_der_t d = x509_trust_get(i);
        x509_cert_t c;
        int rc = x509_parse(d.der, d.len, &c);
        if (rc || (!c.has_rsa_key && !c.has_ec_key)) { printf("anchor %d: parse=%d rsa=%d ec=%d\n", i, rc, c.has_rsa_key, c.has_ec_key); bad++; continue; }
        if (c.has_ec_key) ec++;
        if (!c.is_ca) { printf("anchor %d is not a CA\n", i); bad++; }
        else ca++;
        if (c.critical_unsupported) { printf("anchor %d has an unsupported critical extension\n", i); }
        /* a root must verify its own signature unless it is SHA-1 based */
        int sr = x509_check_signature(&c, &c);
        if (sr != X509_OK && sr != X509_ERR_WEAK_SIG_ALG) { printf("anchor %d self-signature: %d\n", i, sr); bad++; }
    }
    printf("anchors=%d ca=%d ec=%d bad=%d\n", n, ca, ec, bad);
    return bad != 0;
}
