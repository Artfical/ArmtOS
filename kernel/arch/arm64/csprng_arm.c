#include "csprng.h"
#include "chacha20.h"
#include "sha256.h"
#include "net.h"
#include "string.h"
#include "debugmon.h"
#include "bcm.h"

/*
 * The generator is the x86 kernel's (kernel/net/csprng.c): ChaCha20 with fast key erasure,
 * re-keyed from fresh timing on every call. Only the seed differs -- the board has no
 * RDRAND or PIT, so the entropy comes from the SoC's hardware random number generator and
 * from timing jitter on the 64-bit generic counter.
 */

static uint8_t g_key[32];
static uint32_t g_ctr;
static int g_seeded;

static inline uint64_t rd_tsc(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/* BCM2835 hardware RNG: control at +0, status at +4 (words available in bits 31:24), data at +8. */
#define RNG_BASE   (bcm_periph_base + 0x104000)
static int hw_rng_ok = -1;

static int hw_rng_word(uint32_t *v)
{
    if (hw_rng_ok == 0) return 0;
    if (hw_rng_ok < 0) {
        uint32_t st;
        if (bcm_generation >= 5 || mmio_probe(RNG_BASE + 4, &st) != 0) { hw_rng_ok = 0; return 0; }
        mmio_write(RNG_BASE + 0x10, 0x1);              /* mask its interrupt */
        mmio_write(RNG_BASE + 4, 0x40000);             /* warm-up count, as Linux sets it */
        mmio_write(RNG_BASE + 0, 1);                   /* enable */
        hw_rng_ok = 1;
    }
    for (int i = 0; i < 100000; i++) {
        if ((mmio_read(RNG_BASE + 4) >> 24) != 0) { *v = mmio_read(RNG_BASE + 8); return 1; }
    }
    return 0;
}

static void wipe(void *p, int n)
{
    volatile uint8_t *q = (volatile uint8_t *)p;
    while (n--) *q++ = 0;
}

/* key = SHA-256(key || data || fresh timing) */
static void mix(const void *data, int len)
{
    sha256_t s;
    uint8_t out[32];
    uint64_t t = rd_tsc();
    uint32_t up = debugmon_uptime_ms();
    sha256_init(&s);
    sha256_update(&s, g_key, 32);
    if (len > 0) sha256_update(&s, (const uint8_t *)data, (uint32_t)len);
    sha256_update(&s, (const uint8_t *)&t, sizeof(t));
    sha256_update(&s, (const uint8_t *)&up, sizeof(up));
    sha256_update(&s, (const uint8_t *)&g_ctr, sizeof(g_ctr));
    sha256_final(&s, out);
    memcpy(g_key, out, 32);
    wipe(out, sizeof(out));
}

static void seed(void)
{
    sha256_t s;
    uint8_t out[32];
    sha256_init(&s);

    for (int i = 0; i < 16; i++) {
        uint32_t v;
        if (hw_rng_word(&v)) sha256_update(&s, (const uint8_t *)&v, 4);
    }
    /* Timing jitter: the counter delta around a device register read varies with bus and
     * interrupt timing. Weak per sample, so take many. */
    for (int i = 0; i < 1024; i++) {
        uint64_t t0 = rd_tsc();
        uint32_t r = mmio_read(bcm_periph_base + 0x3004);      /* the free-running system timer */
        uint64_t t1 = rd_tsc();
        uint32_t rec[3] = { (uint32_t)(t1 - t0), (uint32_t)t1, r };
        sha256_update(&s, (const uint8_t *)rec, sizeof(rec));
    }
    uint32_t up = debugmon_uptime_ms();
    uint32_t tk = debugmon_get_tick_count();
    uintptr_t sp = (uintptr_t)&s;
    sha256_update(&s, (const uint8_t *)&up, 4);
    sha256_update(&s, (const uint8_t *)&tk, 4);
    sha256_update(&s, (const uint8_t *)&sp, sizeof(sp));
    sha256_update(&s, net_mac, 6);
    sha256_final(&s, out);
    memcpy(g_key, out, 32);
    wipe(out, sizeof(out));
    g_seeded = 1;
}

void csprng_add_entropy(const void *data, int len)
{
    if (!g_seeded) seed();
    mix(data, len);
}

void csprng_fill(uint8_t *buf, int len)
{
    if (len <= 0) return;
    if (!g_seeded) seed();
    mix(0, 0);                       /* fold in fresh timing before every request */
    g_ctr++;

    uint8_t k0[32], blk[64];
    static const uint8_t nonce[12] = { 0 };
    memcpy(k0, g_key, 32);
    /* Block 0 becomes the next key (fast key erasure: past outputs cannot be
     * recomputed from the state after this call); the rest is output. */
    chacha20_block(k0, 0, nonce, blk);
    memcpy(g_key, blk, 32);
    int off = 0;
    int n = len < 32 ? len : 32;
    memcpy(buf, blk + 32, (size_t)n);
    off = n;
    for (uint32_t c = 1; off < len; c++) {
        chacha20_block(k0, c, nonce, blk);
        n = len - off < 64 ? len - off : 64;
        memcpy(buf + off, blk, (size_t)n);
        off += n;
    }
    wipe(k0, sizeof(k0));
    wipe(blk, sizeof(blk));
}

uint32_t csprng_u32(void)
{
    uint32_t v;
    csprng_fill((uint8_t *)&v, 4);
    return v;
}
