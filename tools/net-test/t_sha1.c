/* Host test: SHA-1 over many lengths, printed for t_sha1.py to compare with hashlib. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "sha1.h"
int main(void) {
    for (int len = 0; len <= 200; len++) {
        uint8_t *m = malloc((size_t)len + 1);
        for (int i = 0; i < len; i++) m[i] = (uint8_t)(i * 11 + len);
        uint8_t d[20], c[20];
        sha1_hash(m, (uint32_t)len, d);
        sha1_t s; sha1_init(&s);
        for (int o = 0; o < len; o += 7) sha1_update(&s, m + o, (uint32_t)((len - o) < 7 ? (len - o) : 7));
        sha1_final(&s, c);
        if (memcmp(d, c, 20)) { printf("chunk mismatch at %d\n", len); return 1; }
        printf("%d ", len);
        for (int i = 0; i < 20; i++) printf("%02x", d[i]);
        printf("\n");
        free(m);
    }
    return 0;
}
