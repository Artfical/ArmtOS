#include <stdint.h>
#include "uart.h"
#include "irq.h"
#include "heap.h"
#include "string_arm.h"
#include "ethdev.h"
#include "compat/cmos.h"
#include "compat/klog.h"

/* the x86 kernel's network stack, whose headers share names with this directory's */
#include "net.h"
#include "nic.h"
#include "dhcp.h"
#include "dns.h"
#include "icmp.h"
#include "http.h"
#include "https.h"
#include "tls.h"
#include "udp.h"
#include "tcp.h"
#include "websocket.h"
#include "route.h"
#include "csprng.h"

/*
 * Shell commands for the network stack: bring it up, look at it, and use every layer of it
 * (ARP, ICMP, UDP, DHCP, DNS, TCP, HTTP, TLS, WebSocket, SNTP). `nettest` runs the lot and
 * prints one PASS or FAIL line per layer, which is what the emulator tests read.
 */

static int net_up;

static void put_ip(uint32_t ip)
{
    for (int i = 0; i < 4; i++) {
        uart_put_dec((ip >> (8 * i)) & 0xFF);
        if (i < 3) uart_putc('.');
    }
}

static void put_mac(const uint8_t *m)
{
    for (int k = 0; k < 6; k++) {
        uart_putc("0123456789abcdef"[m[k] >> 4]);
        uart_putc("0123456789abcdef"[m[k] & 0xF]);
        if (k < 5) uart_putc(':');
    }
}

static int ensure_net(void)
{
    if (net_up) return 0;
    uart_puts("net: starting\n");
    net_init();
    if (!nic_send) return -1;
    net_up = 1;
    return 0;
}

static int parse_ip(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return -1;
        unsigned n = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (unsigned)(*s++ - '0'); if (n > 255) return -1; }
        v |= n << (8 * i);
        if (i < 3) { if (*s++ != '.') return -1; }
    }
    if (*s) return -1;
    *out = v;
    return 0;
}

static int resolve(const char *host, uint32_t *ip)
{
    if (parse_ip(host, ip) == 0) return 0;
    int rc = dns_resolve(host, ip);
    if (rc != 0) {
        uart_puts("  dns: ");
        uart_puts(dns_strerror(rc));
        uart_puts("\n");
        return -1;
    }
    return 0;
}

static void show_status(void)
{
    uart_puts("  driver  ");
    uart_puts(nic_driver_name);
    uart_puts("\n  mac     ");
    put_mac(net_mac);
    uart_puts("\n  ip      ");   put_ip(net_ip);
    uart_puts("\n  gateway "); put_ip(net_gateway);
    uart_puts("\n  dns     "); put_ip(net_dns);
    uart_puts("\n  netmask "); put_ip(net_netmask);
    uart_puts("\n  rx/tx   ");
    uart_put_dec(nic_rx_packets); uart_puts(" / "); uart_put_dec(nic_tx_packets);
    uart_puts(" packets\n");
    cmos_time_t t;
    cmos_get_time(&t);
    uart_puts("  time    ");
    uart_put_dec((uint64_t)t.year); uart_putc('-'); uart_put_dec((uint64_t)t.month);
    uart_putc('-'); uart_put_dec((uint64_t)t.day); uart_putc(' ');
    uart_put_dec((uint64_t)t.hour); uart_putc(':'); uart_put_dec((uint64_t)t.minute);
    uart_puts(wallclock_is_set() ? " UTC (from SNTP)\n" : " UTC (build date; run ntp)\n");
}

/* One SNTP exchange; sets the wall clock. 0 on success. */
static int sntp(uint32_t server)
{
    uint8_t pkt[48];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;                                  /* LI 0, version 3, mode 3 (client) */
    uint16_t port = (uint16_t)(40000 + (csprng_u32() & 0x3FFF));
    if (udp_open(port) < 0) return -1;
    int rc = -1;
    for (int attempt = 0; attempt < 3 && rc != 0; attempt++) {
        if (udp_send(server, 123, port, pkt, sizeof(pkt)) != 0) continue;
        uint32_t deadline = (uint32_t)(timer_us() / 1000) + 2000;
        while ((uint32_t)(timer_us() / 1000) < deadline) {
            net_poll();
            uint8_t resp[128];
            uint32_t sip; uint16_t sp;
            int n = udp_listen(port, resp, sizeof(resp), &sip, &sp);
            if (n >= 48) {
                uint32_t secs = ((uint32_t)resp[40] << 24) | ((uint32_t)resp[41] << 16) |
                                ((uint32_t)resp[42] << 8) | resp[43];
                if (secs > 2208988800u) {
                    wallclock_set((uint64_t)secs - 2208988800u);
                    rc = 0;
                }
                break;
            }
        }
    }
    udp_close(port);
    return rc;
}

static void print_head(const uint8_t *r, int n)
{
    /* the status line and the first headers */
    int lines = 0;
    for (int i = 0; i < n && i < 400 && lines < 4; i++) {
        if (r[i] == '\r') continue;
        uart_putc((char)r[i]);
        if (r[i] == '\n') lines++;
    }
}

#define PASS(name, detail) do { uart_puts("PASS  "); uart_puts(name); uart_puts("  "); uart_puts(detail); uart_puts("\n"); passed++; } while (0)
#define FAIL(name, detail) do { uart_puts("FAIL  "); uart_puts(name); uart_puts("  "); uart_puts(detail); uart_puts("\n"); failed++; } while (0)

static int http_status(const uint8_t *r, int n)
{
    if (n < 12 || memcmp(r, "HTTP/1.", 7) != 0) return -1;
    return (r[9] - '0') * 100 + (r[10] - '0') * 10 + (r[11] - '0');
}

static void test_https(const char *host, int *pp, int *fp)
{
    int passed = *pp, failed = *fp;
    char name[40] = "https ";
    strcpy(name + 6, host);
    uint32_t ip;
    if (resolve(host, &ip) != 0) { FAIL(name, "dns"); goto out; }
    uint8_t *buf = malloc(65536);
    if (!buf) { FAIL(name, "out of memory"); goto out; }
    int n = https_get(ip, host, 443, "/", buf, 65536);
    if (n > 0) {
        int st = http_status(buf, n);
        if (st > 0) {
            char d[48] = "HTTP ";
            d[5] = (char)('0' + st / 100); d[6] = (char)('0' + st / 10 % 10); d[7] = (char)('0' + st % 10);
            strcpy(d + 8, ", ");
            int l = 10, v = n, digits = 1;
            for (int t = v; t >= 10; t /= 10) digits++;
            for (int k = digits - 1; k >= 0; k--) { d[l + k] = (char)('0' + v % 10); v /= 10; }
            strcpy(d + l + digits, " bytes, certificate chain verified");
            PASS(name, d);
        } else FAIL(name, "not an HTTP response");
    } else {
        static char why[64];
        const char *m = n <= -50 && n >= -52 ? tls_connect_strerror(n) : (n < 0 ? http_strerror(n) : "empty response");
        strcpy(why, "rc=-");
        int v = n < 0 ? -n : 0, l = 4, dg = 1;
        for (int t = v; t >= 10; t /= 10) dg++;
        for (int k = dg - 1; k >= 0; k--) { why[l + k] = (char)('0' + v % 10); v /= 10; }
        l += dg; why[l++] = ' ';
        strncpy(why + l, m, sizeof(why) - 1 - (size_t)l); why[sizeof(why) - 1] = 0;
        FAIL(name, why);
    }
    free(buf);
out:
    *pp = passed; *fp = failed;
}

static void nettest(int argc, char **argv)
{
    int passed = 0, failed = 0;
    if (ensure_net() != 0) { uart_puts("FAIL  nic  no network adapter found\n"); return; }
    PASS("nic", nic_driver_name);

    if (net_ip != 0 && net_ip != IP4(10,0,2,15)) PASS("dhcp", "lease acquired");
    else if (net_ip == IP4(10,0,2,15)) PASS("dhcp", "address 10.0.2.15 (lease or the slirp default)");
    else FAIL("dhcp", "no address");

    int rc = icmp_ping(net_gateway);
    if (rc == 0) PASS("arp+icmp", "gateway answers"); else FAIL("arp+icmp", "gateway does not answer");

    int local = argc >= 2 && !strcmp(argv[1], "local");   /* offline subset: nothing beyond the emulator */
    const char *wsport = argc >= (local ? 3 : 2) ? argv[local ? 2 : 1] : 0;
    if (!local) {
    uint32_t ip;
    if (resolve("example.com", &ip) == 0) { PASS("dns", "example.com resolves"); } else FAIL("dns", "example.com did not resolve");

    uint32_t ntp;
    if (resolve("time.cloudflare.com", &ntp) == 0 && sntp(ntp) == 0) PASS("sntp", "wall clock set");
    else if (resolve("pool.ntp.org", &ntp) == 0 && sntp(ntp) == 0) PASS("sntp", "wall clock set (pool.ntp.org)");
    else FAIL("sntp", "no time server answered (TLS certificate dates will be unchecked-by-build-date)");

    uint8_t *buf = malloc(65536);
    if (buf) {
        int n = resolve("example.com", &ip) == 0 ? http_get(ip, "example.com", 80, "/", buf, 65536) : -1;
        int st = n > 0 ? http_status(buf, n) : -1;
        if (st == 200) PASS("tcp+http", "example.com 200 OK"); else if (st > 0) PASS("tcp+http", "example.com answered (redirect/other status)");
        else FAIL("tcp+http", "no HTTP response");
        free(buf);
    }

    test_https("example.com", &passed, &failed);        /* RSA/ECDSA chain, ECDHE + AES-GCM */
    test_https("github.com", &passed, &failed);         /* ECDSA P-256 */
    test_https("www.cloudflare.com", &passed, &failed); /* ChaCha20-Poly1305 / ECDSA */
    test_https("www.google.com", &passed, &failed);

    }

    if (wsport) {                                      /* nettest [local] <ws-port>: a WebSocket echo server on the host */
        uint32_t host = IP4(10,0,2,2);
        uint16_t port = 0;
        for (const char *p = wsport; *p >= '0' && *p <= '9'; p++) port = (uint16_t)(port * 10 + (*p - '0'));
        ws_t *ws = malloc(sizeof(ws_t));
        if (ws) {
            memset(ws, 0, sizeof(*ws));
            if (ws_connect(ws, host, "10.0.2.2", port, "/", 0) == 0) {
                int opc = 0;
                uint8_t rx[128];
                ws_send_text(ws, "hello from the board");
                int n = ws_recv(ws, rx, sizeof(rx) - 1, &opc, 3000);
                if (n == 20 && memcmp(rx, "hello from the board", 20) == 0) PASS("websocket", "text frame echoed back");
                else FAIL("websocket", "no echo");
                ws_close(ws);
            } else FAIL("websocket", "handshake failed");
            free(ws);
        }
    }

    uart_puts("nettest: ");
    uart_put_dec((uint64_t)passed); uart_puts(" passed, ");
    uart_put_dec((uint64_t)failed); uart_puts(" failed\n");
}

/* Returns 1 if the command was a network command. */
int net_command(int argc, char **argv)
{
    const char *c = argv[0];
    if (!strcmp(c, "net") || !strcmp(c, "ifconfig")) {
        if (ensure_net() != 0) { uart_puts("net: no network adapter this kernel can drive\n"); return 1; }
        show_status();
    } else if (!strcmp(c, "netlog")) {
        net_verbose = argc >= 2 && !strcmp(argv[1], "on");
        uart_puts(net_verbose ? "netlog on\n" : "netlog off\n");
    } else if (!strcmp(c, "ping") && argc >= 2) {
        if (ensure_net() != 0) { uart_puts("net: no network adapter\n"); return 1; }
        uint32_t ip;
        if (resolve(argv[1], &ip) != 0) return 1;
        for (int i = 0; i < 3; i++) {
            uint64_t t0 = timer_us();
            int rc = icmp_ping(ip);
            if (rc == 0) {
                uart_puts("reply from "); put_ip(ip); uart_puts(" time ");
                uart_put_dec((timer_us() - t0) / 1000); uart_puts(" ms\n");
            } else uart_puts(rc < 0 ? "no reply\n" : "ping failed\n");
            timer_delay_ms(300);
        }
    } else if (!strcmp(c, "dns") && argc >= 2) {
        if (ensure_net() != 0) { uart_puts("net: no network adapter\n"); return 1; }
        uint32_t ip;
        if (resolve(argv[1], &ip) == 0) { put_ip(ip); uart_puts("\n"); }
    } else if (!strcmp(c, "ntp")) {
        if (ensure_net() != 0) { uart_puts("net: no network adapter\n"); return 1; }
        uint32_t ip;
        if (resolve(argc >= 2 ? argv[1] : "time.cloudflare.com", &ip) == 0 && sntp(ip) == 0) show_status();
        else uart_puts("ntp: no answer\n");
    } else if ((!strcmp(c, "wget") || !strcmp(c, "https")) && argc >= 2) {
        if (ensure_net() != 0) { uart_puts("net: no network adapter\n"); return 1; }
        int tls = !strcmp(c, "https");
        uint32_t ip;
        if (resolve(argv[1], &ip) != 0) return 1;
        uint8_t *buf = malloc(65536);
        if (!buf) { uart_puts("out of memory\n"); return 1; }
        const char *path = argc >= 3 ? argv[2] : "/";
        int n = tls ? https_get(ip, argv[1], 443, path, buf, 65536)
                    : http_get(ip, argv[1], 80, path, buf, 65536);
        if (n > 0) {
            print_head(buf, n);
            uart_put_dec((uint64_t)n); uart_puts(" bytes\n");
        } else {
            uart_puts(tls ? "https: " : "http: ");
            uart_puts(n <= -50 && n >= -52 ? tls_connect_strerror(n) : n < 0 ? http_strerror(n) : "empty response");
            if (tls && tls_cert_error_detail()[0]) { uart_puts(" ("); uart_puts(tls_cert_error_detail()); uart_puts(")"); }
            uart_puts("\n");
        }
        free(buf);
    } else if (!strcmp(c, "ws") && argc >= 4) {
        if (ensure_net() != 0) { uart_puts("net: no network adapter\n"); return 1; }
        uint32_t ip;
        if (resolve(argv[1], &ip) != 0) return 1;
        uint16_t port = 0;
        for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) port = (uint16_t)(port * 10 + (*p - '0'));
        ws_t *ws = malloc(sizeof(ws_t));
        if (!ws) return 1;
        memset(ws, 0, sizeof(*ws));
        if (ws_connect(ws, ip, argv[1], port, argv[3], port == 443) != 0) { uart_puts("ws: handshake failed\n"); free(ws); return 1; }
        ws_send_text(ws, argc >= 5 ? argv[4] : "hello");
        int opc = 0; uint8_t rx[256];
        int n = ws_recv(ws, rx, sizeof(rx) - 1, &opc, 3000);
        if (n >= 0) { rx[n] = 0; uart_puts("ws: "); uart_puts((char *)rx); uart_puts("\n"); } else uart_puts("ws: nothing received\n");
        ws_close(ws);
        free(ws);
    } else if (!strcmp(c, "nettest")) {
        nettest(argc - 0, argv);
    } else {
        return 0;
    }
    return 1;
}
