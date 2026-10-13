#include "commands.h"
#include "proc.h"
#include "auth.h"
#include "terminal.h"
#include "string.h"
#include "ramfs.h"
#include "dns.h"
#include "icmp.h"
#include "http.h"
#include "https.h"
#include "websocket.h"
#include "tls.h"
#include "x509.h"
#include "tsharp.h"
#include "micropython.h"
#include "memory.h"
#include "net.h"
#include "nic.h"
#include "fsbridge.h"
#include "usermode.h"
#include "paging.h"

static void print_ip_dotted(uint32_t ip)
{
    char buf[16]; int i = 0;
    uint8_t b[4];
    b[0] = ip & 0xFF; b[1] = (ip >> 8) & 0xFF;
    b[2] = (ip >> 16) & 0xFF; b[3] = (ip >> 24) & 0xFF;
    for (int n = 0; n < 4; n++) {
        uint8_t v = b[n];
        if (v >= 100) buf[i++] = '0' + v / 100;
        if (v >= 10)  buf[i++] = '0' + (v / 10) % 10;
        buf[i++] = '0' + v % 10;
        if (n < 3) buf[i++] = '.';
    }
    buf[i] = '\0';
    terminal_writestring(buf);
}

static void print_hex_byte(uint8_t v)
{
    static const char hex[] = "0123456789abcdef";
    terminal_putchar(hex[v >> 4]);
    terminal_putchar(hex[v & 0xF]);
}

static void print_num(uint32_t n)
{
    char buf[12];
    int i = 11;
    buf[11] = '\0';
    if (n == 0) { buf[10] = '0'; terminal_writestring(buf + 10); return; }
    while (n > 0 && i > 0) { buf[--i] = '0' + (n % 10); n /= 10; }
    terminal_writestring(buf + i);
}

/* Shows which NIC driver actually attached (or "None" if every probe in
 * nic_init() failed) and the current rx/tx counters -- without this,
 * telling "no NIC found" apart from "a NIC attached but ARP/ICMP just
 * isn't getting replies" required scrolling back through dmesg to find
 * the probe lines. */
void cmd_ifconfig(int argc, char **args)
{
    (void)argc; (void)args;
    terminal_writestring("driver: ");
    terminal_writestring(nic_driver_name);
    terminal_writestring("\nmac:    ");
    for (int i = 0; i < 6; i++) {
        print_hex_byte(net_mac[i]);
        if (i < 5) terminal_putchar(':');
    }
    terminal_writestring("\nip:     ");      print_ip_dotted(net_ip);
    terminal_writestring("\nmask:   ");      print_ip_dotted(net_netmask);
    terminal_writestring("\ngateway:");      print_ip_dotted(net_gateway);
    terminal_writestring("\ndns:    ");      print_ip_dotted(net_dns);
    terminal_writestring("\nrx:     ");
    print_num(nic_rx_packets);
    terminal_writestring(" packets, ");
    print_num(nic_rx_bytes);
    terminal_writestring(" bytes\ntx:     ");
    print_num(nic_tx_packets);
    terminal_writestring(" packets, ");
    print_num(nic_tx_bytes);
    terminal_writestring(" bytes\n");
}

/* Strict dotted-quad: exactly four decimal parts of 1-3 digits, each 0-255. The
 * old parser masked every part with 0xFF (so '300.1.1.1' became 44.1.1.1), let a
 * fifth part shift into undefined behaviour and accepted empty parts. Returns 0
 * and the address in wire byte order, or -1. */
static int parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t ip = 0;
    int part = 0, val = 0, digits = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (++digits > 3 || val > 255) return -1;
        } else if (*s == '.' || *s == '\0') {
            if (digits == 0 || part > 3) return -1;
            ip |= (uint32_t)val << (8 * part);
            part++; val = 0; digits = 0;
            if (*s == '\0') break;
        } else {
            return -1;
        }
    }
    if (part != 4) return -1;
    *out = ip;
    return 0;
}

/* Port number: decimal digits only, 1-65535 (0 allowed when allow_zero). The
 * old loops multiplied whatever characters they met, so '99999' wrapped to
 * another port and 'abc' became a random one. */
static int parse_u16(const char *s, uint16_t *out, int allow_zero)
{
    uint32_t v = 0;
    int digits = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (uint32_t)(*s - '0');
        if (++digits > 5 || v > 65535) return -1;
    }
    if (digits == 0 || (v == 0 && !allow_zero)) return -1;
    *out = (uint16_t)v;
    return 0;
}

#define NEED_IP(var, str) uint32_t var; \
    if (parse_ipv4((str), &(var)) != 0) { terminal_writestring("invalid IP address: "); terminal_writestring(str); terminal_putchar('\n'); return; }
#define NEED_PORT(var, str) uint16_t var; \
    if (parse_u16((str), &(var), 0) != 0) { terminal_writestring("invalid port (1-65535): "); terminal_writestring(str); terminal_putchar('\n'); return; }

void cmd_ping(int argc, char **args)
{
    if (argc < 2) { terminal_writestring("usage: ping <ip> or <hostname>\n"); return; }
    uint32_t ip;
    int is_ip = 1;
    for (char *p = args[1]; *p; p++)
        if ((*p < '0' || *p > '9') && *p != '.') { is_ip = 0; break; }
    if (is_ip) {
        if (parse_ipv4(args[1], &ip) != 0) { terminal_writestring("ping: invalid IP address\n"); return; }
    }
    else {
        terminal_writestring("Resolving... ");
        int rc = dns_resolve(args[1], &ip);
        if (rc != 0) {
            terminal_writestring("FAILED (");
            terminal_writestring(dns_strerror(rc));
            terminal_writestring(")\n");
            return;
        }
        terminal_writestring("OK\n");
    }
    terminal_writestring("Pinging... ");
    int prc = icmp_ping(ip);
    if (prc == 0) terminal_writestring("Reply received\n");
    else {
        terminal_writestring("No reply (");
        terminal_writestring(icmp_ping_strerror(prc));
        terminal_writestring(")\n");
    }
}

/* Case-insensitive substring search -- HTTP header names are
 * case-insensitive on the wire, and servers disagree on how they
 * capitalize "Content-Type". */
static const char *ci_strstr(const char *hay, const char *needle)
{
    int nlen = 0;
    while (needle[nlen]) nlen++;
    for (const char *p = hay; *p; p++) {
        int k = 0;
        while (k < nlen && p[k] && (p[k] | 0x20) == (needle[k] | 0x20)) k++;
        if (k == nlen) return p;
    }
    return 0;
}

/* Maps the response's Content-Type header to a file extension, so a
 * URL with no filename in its path (e.g. "wget http://site.com/") or
 * an extensionless one saves as something recognizable instead of a
 * bare "downloaded" with no type info at all. Empty string if the
 * header's missing or the type isn't one of the common ones below. */
static const char *ext_for_content_type(const char *resp)
{
    const char *hdr_end = strstr(resp, "\r\n\r\n");
    const char *ct = ci_strstr(resp, "content-type:");
    if (!ct || (hdr_end && ct > hdr_end)) return "";
    ct += 13;
    while (*ct == ' ') ct++;
    if (!strncmp(ct, "text/html", 9))            return ".html";
    if (!strncmp(ct, "application/json", 16))    return ".json";
    if (!strncmp(ct, "text/css", 8))             return ".css";
    if (!strncmp(ct, "application/javascript", 22)) return ".js";
    if (!strncmp(ct, "text/javascript", 15))     return ".js";
    if (!strncmp(ct, "image/png", 9))            return ".png";
    if (!strncmp(ct, "image/jpeg", 10))          return ".jpg";
    if (!strncmp(ct, "image/gif", 9))            return ".gif";
    if (!strncmp(ct, "image/svg", 9))            return ".svg";
    if (!strncmp(ct, "application/pdf", 15))     return ".pdf";
    if (!strncmp(ct, "text/xml", 8))              return ".xml";
    if (!strncmp(ct, "application/xml", 15))     return ".xml";
    if (!strncmp(ct, "text/plain", 10))          return ".txt";
    return "";
}

void cmd_wget(int argc, char **args)
{
    const char *url = 0;
    int insecure = 0;
    for (int a = 1; a < argc; a++) {
        if (strcmp(args[a], "-k") == 0 || strcmp(args[a], "--insecure") == 0) insecure = 1;
        else if (!url) url = args[a];
    }
    if (!url) {
        terminal_writestring("usage: wget [-k] <url>\n");
        terminal_writestring("  e.g. wget http://example.com/file\n");
        terminal_writestring("       wget https://example.com/file\n");
        terminal_writestring("  -k   do not verify the HTTPS server certificate (unsafe)\n");
        return;
    }

    /* Redirects are followed, because most sites answer the address a person actually types
     * with one: http to https, bare domain to www. Without this they look simply broken. */
    char urlbuf[512];
    strncpy(urlbuf, url, sizeof(urlbuf) - 1);
    urlbuf[sizeof(urlbuf) - 1] = 0;
    int hops = 0;

follow:;
    url = urlbuf;
    int use_tls = 0;
    if (strncmp(url, "https://", 8) == 0) { use_tls = 1; url += 8; }
    else if (strncmp(url, "http://", 7) == 0) { url += 7; }
    else { terminal_writestring("wget: Only http:// and https:// supported\n"); return; }

    char host[256], path[256];
    int i = 0, j = 0;
    while (*url && *url != '/' && *url != ':' && i < 255) host[i++] = *url++;
    host[i] = '\0';

    uint16_t port = use_tls ? 443 : 80;
    if (*url == ':') {
        url++;
        uint32_t pv = 0; int pd = 0;
        while (*url >= '0' && *url <= '9') { pv = pv * 10 + (uint32_t)(*url - '0'); url++; if (++pd > 5) break; }
        if (pd == 0 || pd > 5 || pv == 0 || pv > 65535 || (*url != '/' && *url != '\0')) {
            terminal_writestring("wget: invalid port in the URL (1-65535)\n");
            return;
        }
        port = (uint16_t)pv;
    }

    if (*url == '/') { while (*url && j < 255) path[j++] = *url++; path[j] = '\0'; }
    else { path[0] = '/'; path[1] = '\0'; }

    const char *base_fname = path;
    for (const char *p = path; *p; p++) if (*p == '/') base_fname = p + 1;
    if (*base_fname == '\0') base_fname = "downloaded";
    char fname[300];
    int fi = 0;
    while (base_fname[fi] && fi < 250) { fname[fi] = base_fname[fi]; fi++; }
    fname[fi] = '\0';

    uint32_t ip;
    int host_is_ip = 1;
    for (const char *p = host; *p; p++)
        if ((*p < '0' || *p > '9') && *p != '.') { host_is_ip = 0; break; }

    if (host_is_ip) {
        if (parse_ipv4(host, &ip) != 0) { terminal_writestring("wget: invalid IP address\n"); return; }
    } else {
        terminal_writestring("Resolving... ");
        int rc = dns_resolve(host, &ip);
        if (rc != 0) {
            terminal_writestring("FAILED (");
            terminal_writestring(dns_strerror(rc));
            terminal_writestring(")\n");
            return;
        }
        terminal_writestring("OK\n");
    }
    terminal_writestring(use_tls ? "Connecting (TLS)... " : "Connecting... ");

    /* A real page can easily run into the hundreds of KB -- the old
     * 4096-byte stack array silently truncated anything past that.
     * A buffer that size can't just be made a bigger stack array (the
     * kernel stack is nowhere near large enough), so heap-allocate it
     * instead. Capped well under KERNEL_HEAP_MAX_SIZE (64MB total, the
     * whole kernel heap's hard ceiling, not just this buffer's) so one
     * wget doesn't leave nothing for anything else running. */
    #define WGET_BUF_SIZE (32 * 1024 * 1024)
    uint8_t *resp = (uint8_t *)malloc(WGET_BUF_SIZE);
    if (!resp) { terminal_writestring("FAILED (out of memory)\n"); return; }
    int n;
    if (use_tls) {
        int verify_before = tls_get_verify();
        if (insecure) tls_set_verify(0);
        n = https_get(ip, host, port, path, resp, WGET_BUF_SIZE - 1);
        tls_set_verify(verify_before);
        if (n < 0) {
            terminal_writestring("FAILED (");
            terminal_writestring(tls_connect_strerror(n));
            terminal_writestring(")\n");
            if (n == TLS_ERR_CERT) terminal_writestring("(use wget -k to skip certificate checks)\n");
            free(resp);
            return;
        }
        if (n == 0) { terminal_writestring("FAILED (connected, but received no data)\n"); free(resp); return; }
    } else {
        n = http_get(ip, host, port, path, resp, WGET_BUF_SIZE - 1);
        if (n < 0) {
            terminal_writestring("FAILED (");
            terminal_writestring(http_strerror(n));
            terminal_writestring(")\n");
            free(resp);
            return;
        }
        if (n == 0) { terminal_writestring("FAILED (connected, but received no data)\n"); free(resp); return; }
    }
    resp[n] = '\0';
    terminal_writestring("OK (");
    print_num(n);
    terminal_writestring(" bytes)\n");

    {
        /* 3xx with a Location header: go there instead, up to a few hops so a loop of
         * redirects cannot run forever. */
        int code = 0;
        if (n > 12 && strncmp((const char *)resp, "HTTP/1.", 7) == 0)
            code = (resp[9] - '0') * 100 + (resp[10] - '0') * 10 + (resp[11] - '0');
        if (code >= 300 && code <= 399) {
            const char *loc = http_header_value((const char *)resp, n, "location:");
            if (loc && hops < 8) {
                char next[512];
                int ln = 0;
                while (loc[ln] && loc[ln] != '\r' && loc[ln] != '\n' && ln < (int)sizeof(next) - 1) { next[ln] = loc[ln]; ln++; }
                next[ln] = 0;
                if (next[0] == '/') {                       /* same site, different path */
                    int k = 0;
                    const char *scheme = use_tls ? "https://" : "http://";
                    while (*scheme && k < (int)sizeof(urlbuf) - 1) urlbuf[k++] = *scheme++;
                    for (int h = 0; host[h] && k < (int)sizeof(urlbuf) - 1; h++) urlbuf[k++] = host[h];
                    if (port != (use_tls ? 443 : 80) && k < (int)sizeof(urlbuf) - 8) {
                        urlbuf[k++] = ':';
                        char pb[8];
                        int pl = 0, pv = port;
                        while (pv) { pb[pl++] = (char)('0' + pv % 10); pv /= 10; }
                        while (pl > 0 && k < (int)sizeof(urlbuf) - 1) urlbuf[k++] = pb[--pl];
                    }
                    for (int h = 0; next[h] && k < (int)sizeof(urlbuf) - 1; h++) urlbuf[k++] = next[h];
                    urlbuf[k] = 0;
                } else {
                    strncpy(urlbuf, next, sizeof(urlbuf) - 1);
                    urlbuf[sizeof(urlbuf) - 1] = 0;
                }
                terminal_writestring("-> ");
                terminal_writestring(urlbuf);
                terminal_writestring("\n");
                free(resp);
                hops++;
                goto follow;
            }
        }
    }

    int has_dot = 0;
    for (int k = 0; fname[k]; k++) if (fname[k] == '.') has_dot = 1;
    if (!has_dot) {
        const char *ext = ext_for_content_type((const char *)resp);
        int el = 0;
        while (ext[el] && fi + el < 299) { fname[fi + el] = ext[el]; el++; }
        fname[fi + el] = '\0';
    }

    /* The response still starts with the status line and headers; the
     * saved file must be just the body (the headers were only needed
     * above, for the content type). */
    int body_off = 0;
    for (int k = 0; k + 3 < n; k++) {
        if (resp[k] == '\r' && resp[k + 1] == '\n' && resp[k + 2] == '\r' && resp[k + 3] == '\n') { body_off = k + 4; break; }
    }
    if (ramfs_create(fname) == 0) {
        ramfs_write(fname, (char *)resp + body_off, n - body_off, 0);
        terminal_writestring("Saved to: ");
        terminal_writestring(fname);
        terminal_writestring("\n");
    } else {
        terminal_writestring("(Cannot write, showing content)\n");
        terminal_writestring((char *)resp);
        terminal_putchar('\n');
    }
    free(resp);
}

/* ---- tls: certificate trust management ---- */

static int b64val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodes the base64 text between a PEM header and footer (whitespace ignored).
 * Returns the DER length, or -1. */
static int pem_body_decode(const char *p, int len, uint8_t *out, int cap)
{
    int n = 0, acc = 0, bits = 0;
    for (int i = 0; i < len; i++) {
        if (p[i] == '=') break;
        int v = b64val((unsigned char)p[i]);
        if (v < 0) { if (p[i] == '\n' || p[i] == '\r' || p[i] == ' ' || p[i] == '\t') continue; return -1; }
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)(acc >> bits);
            acc &= (1 << bits) - 1;
        }
    }
    return n;
}

static void tls_print_num(int v) { print_num(v); }

void cmd_tls(int argc, char **args)
{
    if (argc >= 2 && strcmp(args[1], "verify") == 0) {
        if (argc >= 3) {
            if (strcmp(args[2], "on") == 0) tls_set_verify(1);
            else if (strcmp(args[2], "off") == 0) tls_set_verify(0);
            else { terminal_writestring("usage: tls verify [on|off]\n"); return; }
        }
        terminal_writestring(tls_get_verify() ? "certificate verification: on\n"
                                              : "certificate verification: OFF (servers are not authenticated)\n");
        return;
    }
    if (argc >= 2 && strcmp(args[1], "roots") == 0) {
        terminal_writestring("trusted root certificates: ");
        tls_print_num(x509_trust_count());
        terminal_writestring("\n");
        return;
    }
    if (argc >= 3 && strcmp(args[1], "trust") == 0) {
        const char *path = args[2];
        if (!fsbridge_exists(path) || fsbridge_is_dir(path)) { terminal_writestring("tls: file not found\n"); return; }
        uint32_t size = fsbridge_size(path);
        if (size == 0 || size > 256 * 1024) { terminal_writestring("tls: file is empty or too large\n"); return; }
        char *buf = (char *)malloc(size + 1);
        uint8_t *der = (uint8_t *)malloc(X509_MAX_CERT_LEN);
        if (!buf || !der) { free(buf); free(der); terminal_writestring("tls: out of memory\n"); return; }
        if (fsbridge_read(path, buf, size, 0) < 0) { terminal_writestring("tls: read failed\n"); free(buf); free(der); return; }
        buf[size] = 0;
        int added = 0, failed = 0;
        static const char begin[] = "-----BEGIN CERTIFICATE-----";
        static const char end[] = "-----END CERTIFICATE-----";
        char *p = buf;
        int pem = 0;
        for (;;) {
            char *b = strstr(p, begin);
            if (!b) break;
            pem = 1;
            b += sizeof(begin) - 1;
            char *e = strstr(b, end);
            if (!e) { failed++; break; }
            int dl = pem_body_decode(b, (int)(e - b), der, X509_MAX_CERT_LEN);
            if (dl > 0 && x509_trust_add(der, (uint32_t)dl) == X509_OK) added++; else failed++;
            p = e + sizeof(end) - 1;
        }
        if (!pem) {                                   /* a single DER certificate */
            if (size <= X509_MAX_CERT_LEN && x509_trust_add((const uint8_t *)buf, size) == X509_OK) added++; else failed++;
        }
        terminal_writestring("tls: trusted ");
        tls_print_num(added);
        terminal_writestring(" certificate(s)");
        if (failed) { terminal_writestring(", rejected "); tls_print_num(failed); terminal_writestring(" (unparsable, not RSA 2048-4096, or table full)"); }
        terminal_writestring("\n");
        free(buf);
        free(der);
        return;
    }
    terminal_writestring("usage: tls trust <file.pem|file.der>   trust extra root certificate(s) until reboot\n");
    terminal_writestring("       tls verify [on|off]            show or change HTTPS certificate checking\n");
    terminal_writestring("       tls roots                      number of trusted root certificates\n");
}

void cmd_tsharp(int argc, char **args)
{
    if (argc > 1) tsharp_run_file(args[1]);
    else tsharp_run_interactive();
}

void cmd_python(int argc, char **args)
{
    /* args[0]="python", args[1]=script path, args[2:]=script args --
     * sys.argv inside the script is args[1:] (path is sys.argv[0], same
     * as CPython's `python foo.py a b` -> sys.argv == ["foo.py","a","b"]) */
    if (argc > 1) micropython_run_file_argv(args[1], argc - 1, args + 1);
    else micropython_run_repl();
}

/* USER_CODE_BASE / USER_STACK_TOP / USER_CODE_MAX_SIZE come from
 * usermode.h -- see the comment there for why these have to be fixed
 * constants shared between the loader and the SDK's linker script,
 * not computed at boot. */

void cmd_run(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: run <file.t> [&]\n");
        return;
    }
    const char *path = args[1];
    int background = argc > 2 && strcmp(args[argc - 1], "&") == 0;

    int pid = proc_spawn(path);
    if (pid < 0) {
        terminal_writestring("run: ");
        switch (pid) {
        case -1: terminal_writestring("file not found: "); terminal_writestring(path); break;
        case -2: terminal_writestring("bad program file"); break;
        case -3: terminal_writestring("out of memory"); break;
        case -4: terminal_writestring("too many processes"); break;
        case -5: terminal_writestring("permission denied: "); terminal_writestring(path); break;
        default: terminal_writestring("failed"); break;
        }
        terminal_writestring("\n");
        return;
    }
    if (background) {
        terminal_writestring("[process ");
        char num[12];
        sprintf_u32(num, (uint32_t)pid);
        terminal_writestring(num);
        terminal_writestring("]\n");
        return;
    }
    int code = proc_wait((uint32_t)pid);
    (void)code;
    terminal_writestring("\n");
}

/* -----------------------------------------------------------------------
 * New protocol test commands (v0.9.39)
 * ----------------------------------------------------------------------- */
#include "sctp.h"
#include "dccp.h"
#include "udplite.h"
#include "icmpv6.h"
#include "ipsec.h"
#include "ip6.h"
#include "vlan.h"
#include "bridge.h"
#include "bonding.h"
#include "ipx.h"
#include "route.h"
#include "fw.h"
#include "gre.h"
#include "ipip.h"
#include "wgtun.h"
#include "chacha20.h"

void cmd_sctp_connect(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: sctp_connect <ip> <port>\n");
        return;
    }
    NEED_IP(ip, args[1]);
    NEED_PORT(port, args[2]);
    terminal_writestring("SCTP: connecting to ");
    terminal_writestring(args[1]);
    terminal_writestring(":");
    terminal_writestring(args[2]);
    terminal_writestring(" ...\n");
    if (sctp_connect(ip, (uint16_t)port) == 0)
        terminal_writestring("SCTP: association established\n");
    else
        terminal_writestring("SCTP: connection failed\n");
}

void cmd_sctp_send(int argc, char **args)
{
    if (argc < 2) { terminal_writestring("usage: sctp_send <data>\n"); return; }
    if (sctp_send(args[1], strlen(args[1])) == 0)
        terminal_writestring("SCTP: data sent\n");
    else
        terminal_writestring("SCTP: send failed (not connected?)\n");
}

void cmd_sctp_close(int argc, char **args)
{
    (void)argc; (void)args;
    sctp_close();
    terminal_writestring("SCTP: association closed\n");
}

void cmd_dccp_connect(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: dccp_connect <ip> <port> [service-code]\n");
        return;
    }
    NEED_IP(ip, args[1]);
    NEED_PORT(port, args[2]);
    uint32_t service = 0;                            /* DCCP servers listen for one service code */
    if (argc >= 4) for (char *p = args[3]; *p >= '0' && *p <= '9'; p++) service = service * 10 + (uint32_t)(*p - '0');
    terminal_writestring("DCCP: connecting to ");
    terminal_writestring(args[1]);
    terminal_writestring(":");
    terminal_writestring(args[2]);
    terminal_writestring(" ...\n");
    if (dccp_connect_service(ip, (uint16_t)port, service) == 0)
        terminal_writestring("DCCP: connection open\n");
    else
        terminal_writestring("DCCP: connection failed\n");
}

void cmd_dccp_send(int argc, char **args)
{
    if (argc < 2) { terminal_writestring("usage: dccp_send <data>\n"); return; }
    if (dccp_send(args[1], strlen(args[1])) == 0)
        terminal_writestring("DCCP: datagram sent\n");
    else
        terminal_writestring("DCCP: send failed\n");
}

void cmd_udplite_send(int argc, char **args)
{
    if (argc < 4) {
        terminal_writestring("usage: udplite_send <ip> <port> <data> [coverage]\n");
        return;
    }
    NEED_IP(ip, args[1]);
    NEED_PORT(port, args[2]);
    uint16_t coverage = 0;
    if (argc >= 5 && parse_u16(args[4], &coverage, 1) != 0) { terminal_writestring("invalid coverage (0-65535)\n"); return; }
    if (udplite_send(ip, (uint16_t)port, 49136, args[3], strlen(args[3]), coverage) == 0)
        terminal_writestring("UDP-Lite: datagram sent\n");
    else
        terminal_writestring("UDP-Lite: send failed\n");
}

void cmd_ping6(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: ping6 <ipv6-address>\n");
        terminal_writestring("  e.g. ping6 fe80::1\n");
        return;
    }
    uint8_t addr[16];
    if (ip6_parse(args[1], addr) != 0) {
        terminal_writestring("ping6: invalid IPv6 address\n");
        return;
    }
    terminal_writestring("ping6 ");
    terminal_writestring(args[1]);
    terminal_writestring(" ...\n");
    if (icmpv6_ping6(addr) == 0)
        terminal_writestring("Reply received\n");
    else
        terminal_writestring("No reply\n");
}

void cmd_ip6addr(int argc, char **args)
{
    (void)argc; (void)args;
    extern uint8_t net_ip6[16];
    char buf[42];
    ip6_fmt(net_ip6, buf);
    terminal_writestring("IPv6 link-local: ");
    terminal_writestring(buf);
    terminal_writestring("\n");
}

void cmd_ipsec_sa(int argc, char **args)
{
    (void)argc; (void)args;
    ipsec_dump_sa();
}

static int hex_digit(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* up to 8 hex digits -> *out; 0 on success */
static int parse_hex32(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int n = 0;
    for (; *s; s++, n++) {
        int d = hex_digit((unsigned char)*s);
        if (d < 0 || n >= 8) return -1;
        v = (v << 4) | (uint32_t)d;
    }
    if (n == 0) return -1;
    *out = v;
    return 0;
}

/* even number of hex digits -> bytes; returns the byte count or -1 */
static int parse_hex_bytes(const char *s, uint8_t *out, int max)
{
    int n = 0;
    while (s[0]) {
        int hi = hex_digit((unsigned char)s[0]);
        int lo = s[1] ? hex_digit((unsigned char)s[1]) : -1;
        if (hi < 0 || lo < 0 || n >= max) return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
        s += 2;
    }
    return n > 0 ? n : -1;
}

/* ipsec add ah <peer-ip> <spi-hex> <key-hex> / ipsec del <spi-hex> / ipsec list */
void cmd_ipsec(int argc, char **args)
{
    if (argc >= 2 && (strcmp(args[1], "list") == 0 || strcmp(args[1], "sa") == 0)) {
        ipsec_dump_sa();
        return;
    }
    if (argc == 6 && strcmp(args[1], "add") == 0 && strcmp(args[2], "ah") == 0) {
        uint32_t peer;
        if (parse_ipv4(args[3], &peer) != 0) { terminal_writestring("ipsec: invalid peer address\n"); return; }
        uint32_t spi;
        uint8_t key[IPSEC_KEY_MAX];
        int klen = parse_hex_bytes(args[5], key, IPSEC_KEY_MAX);
        if (parse_hex32(args[4], &spi) != 0 || spi == 0) { terminal_writestring("ipsec: SPI must be 1-8 hex digits, not 0\n"); return; }
        if (klen < 16) { terminal_writestring("ipsec: key must be 16-32 bytes (32-64 hex digits)\n"); return; }
        if (ipsec_sa_add(peer, spi, IPPROTO_AH) != 0) { terminal_writestring("ipsec: cannot add SA (table full or already exists)\n"); return; }
        if (ipsec_sa_set_key(peer, spi, key, klen) != 0) { ipsec_sa_remove(spi); terminal_writestring("ipsec: setting the key failed\n"); return; }
        terminal_writestring("ipsec: AH SA added (HMAC-SHA-256-128)\n");
        return;
    }
    if (argc == 3 && strcmp(args[1], "del") == 0) {
        uint32_t spi;
        if (parse_hex32(args[2], &spi) != 0) { terminal_writestring("ipsec: invalid SPI\n"); return; }
        ipsec_sa_remove(spi);
        terminal_writestring("ipsec: SA removed\n");
        return;
    }
    terminal_writestring("usage: ipsec add ah <peer-ip> <spi-hex> <key-hex>   (HMAC-SHA-256 key, 16-32 bytes)\n");
    terminal_writestring("       ipsec del <spi-hex>\n");
    terminal_writestring("       ipsec list\n");
}

/* -----------------------------------------------------------------------
 * VLAN commands
 * vlan add <vid>  /  vlan rm <vid>  /  vlan list
 * ----------------------------------------------------------------------- */
void cmd_vlan(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: vlan add <vid> | vlan rm <vid> | vlan list\n");
        return;
    }
    if (strcmp(args[1], "add") == 0) {
        if (argc < 3) { terminal_writestring("usage: vlan add <vid>\n"); return; }
        int vid = 0;
        for (char *p = args[2]; *p; p++) vid = vid * 10 + (*p - '0');
        if (vlan_add((uint16_t)vid) == 0) {
            terminal_writestring("VLAN ");
            terminal_writestring(args[2]);
            terminal_writestring(" added\n");
        } else {
            terminal_writestring("vlan add: failed (full or invalid vid)\n");
        }
    } else if (strcmp(args[1], "rm") == 0 || strcmp(args[1], "del") == 0) {
        if (argc < 3) { terminal_writestring("usage: vlan rm <vid>\n"); return; }
        int vid = 0;
        for (char *p = args[2]; *p; p++) vid = vid * 10 + (*p - '0');
        if (vlan_remove((uint16_t)vid) == 0) {
            terminal_writestring("VLAN ");
            terminal_writestring(args[2]);
            terminal_writestring(" removed\n");
        } else {
            terminal_writestring("vlan rm: not found\n");
        }
    } else if (strcmp(args[1], "list") == 0) {
        vlan_list();
    } else {
        terminal_writestring("vlan: unknown subcommand. Use add/rm/list\n");
    }
}

/* -----------------------------------------------------------------------
 * Bridge commands
 * bridge create <name>  /  bridge del <name>
 * bridge addif <br> <if>  /  bridge delif <br> <if>
 * bridge list
 * ----------------------------------------------------------------------- */
void cmd_bridge(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: bridge create <name> | bridge del <name>\n");
        terminal_writestring("       bridge addif <br> <if> | bridge delif <br> <if>\n");
        terminal_writestring("       bridge list\n");
        return;
    }
    if (strcmp(args[1], "create") == 0) {
        if (argc < 3) { terminal_writestring("usage: bridge create <name>\n"); return; }
        if (bridge_create(args[2]) == 0) {
            terminal_writestring("bridge ");
            terminal_writestring(args[2]);
            terminal_writestring(" created\n");
        } else {
            terminal_writestring("bridge create: failed\n");
        }
    } else if (strcmp(args[1], "del") == 0) {
        if (argc < 3) { terminal_writestring("usage: bridge del <name>\n"); return; }
        if (bridge_destroy(args[2]) == 0) {
            terminal_writestring("bridge ");
            terminal_writestring(args[2]);
            terminal_writestring(" deleted\n");
        } else {
            terminal_writestring("bridge del: not found\n");
        }
    } else if (strcmp(args[1], "addif") == 0) {
        if (argc < 4) { terminal_writestring("usage: bridge addif <br> <if>\n"); return; }
        if (bridge_add_if(args[2], args[3]) == 0) {
            terminal_writestring(args[3]);
            terminal_writestring(" added to bridge ");
            terminal_writestring(args[2]);
            terminal_putchar('\n');
        } else {
            terminal_writestring("bridge addif: failed\n");
        }
    } else if (strcmp(args[1], "delif") == 0) {
        if (argc < 4) { terminal_writestring("usage: bridge delif <br> <if>\n"); return; }
        if (bridge_remove_if(args[2], args[3]) == 0) {
            terminal_writestring(args[3]);
            terminal_writestring(" removed from bridge ");
            terminal_writestring(args[2]);
            terminal_putchar('\n');
        } else {
            terminal_writestring("bridge delif: failed\n");
        }
    } else if (strcmp(args[1], "list") == 0) {
        bridge_list();
    } else {
        terminal_writestring("bridge: unknown subcommand\n");
    }
}

/* -----------------------------------------------------------------------
 * Bonding commands
 * bond create <name> [failover|balance]
 * bond del <name>
 * bond addif <bond> <if>
 * bond delif <bond> <if>
 * bond failover <name>
 * bond list
 * ----------------------------------------------------------------------- */
void cmd_bond(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: bond create <name> [failover|balance]\n");
        terminal_writestring("       bond del <name>\n");
        terminal_writestring("       bond addif <bond> <if> | bond delif <bond> <if>\n");
        terminal_writestring("       bond failover <name> | bond list\n");
        return;
    }
    if (strcmp(args[1], "create") == 0) {
        if (argc < 3) { terminal_writestring("usage: bond create <name> [failover|balance]\n"); return; }
        bond_mode_t mode = BOND_MODE_FAILOVER;
        if (argc >= 4 && strcmp(args[3], "balance") == 0)
            mode = BOND_MODE_BALANCE;
        if (bond_create(args[2], mode) == 0) {
            terminal_writestring("bond ");
            terminal_writestring(args[2]);
            terminal_writestring(" created (mode=");
            terminal_writestring(mode == BOND_MODE_FAILOVER ? "failover" : "balance");
            terminal_writestring(")\n");
        } else {
            terminal_writestring("bond create: failed\n");
        }
    } else if (strcmp(args[1], "del") == 0) {
        if (argc < 3) { terminal_writestring("usage: bond del <name>\n"); return; }
        if (bond_destroy(args[2]) == 0) {
            terminal_writestring("bond ");
            terminal_writestring(args[2]);
            terminal_writestring(" deleted\n");
        } else {
            terminal_writestring("bond del: not found\n");
        }
    } else if (strcmp(args[1], "addif") == 0) {
        if (argc < 4) { terminal_writestring("usage: bond addif <bond> <if>\n"); return; }
        if (bond_add_slave(args[2], args[3]) == 0) {
            terminal_writestring(args[3]);
            terminal_writestring(" added to bond ");
            terminal_writestring(args[2]);
            terminal_putchar('\n');
        } else {
            terminal_writestring("bond addif: failed\n");
        }
    } else if (strcmp(args[1], "delif") == 0) {
        if (argc < 4) { terminal_writestring("usage: bond delif <bond> <if>\n"); return; }
        if (bond_remove_slave(args[2], args[3]) == 0) {
            terminal_writestring(args[3]);
            terminal_writestring(" removed from bond ");
            terminal_writestring(args[2]);
            terminal_putchar('\n');
        } else {
            terminal_writestring("bond delif: failed\n");
        }
    } else if (strcmp(args[1], "failover") == 0) {
        if (argc < 3) { terminal_writestring("usage: bond failover <name>\n"); return; }
        if (bond_failover(args[2]) != 0)
            terminal_writestring("bond failover: failed (need >= 2 slaves)\n");
    } else if (strcmp(args[1], "list") == 0) {
        bond_list();
    } else {
        terminal_writestring("bond: unknown subcommand\n");
    }
}

/* -----------------------------------------------------------------------
 * IPX command
 * ipx send <dst_node_hex> <data>   e.g. ipx send FFFFFFFFFFFF hello
 * ----------------------------------------------------------------------- */
static int parse_hex_byte(const char *s)
{
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
        else return -1;
    }
    return v;
}

void cmd_ipx(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: ipx send <dst_node_hex12> <data>\n");
        terminal_writestring("  e.g. ipx send FFFFFFFFFFFF hello\n");
        return;
    }
    if (strcmp(args[1], "send") == 0) {
        if (argc < 4) { terminal_writestring("usage: ipx send <node12hex> <data>\n"); return; }
        const char *hex = args[2];
        if (strlen(hex) != 12) { terminal_writestring("ipx: node must be 12 hex chars\n"); return; }
        uint8_t node[6];
        for (int i = 0; i < 6; i++) {
            int b = parse_hex_byte(hex + i * 2);
            if (b < 0) { terminal_writestring("ipx: invalid hex in node\n"); return; }
            node[i] = (uint8_t)b;
        }
        static const uint8_t zero_net[4] = {0,0,0,0};
        const char *data = args[3];
        if (ipx_send(node, zero_net, IPX_SOCK_ECHO, IPX_SOCK_ECHO,
                     IPX_TYPE_ECHO, data, strlen(data)) == 0)
            terminal_writestring("IPX: datagram sent\n");
        else
            terminal_writestring("IPX: send failed\n");
    } else {
        terminal_writestring("ipx: unknown subcommand (try: send)\n");
    }
}

/* -----------------------------------------------------------------------
 * Routing commands
 * route show / route add <dst/prefix> <gw> [metric] [table]
 * route del <dst/prefix> [table]
 * policy add <src/pfx> <dst/pfx> <table> <prio>
 * policy del <prio>
 * policy show
 * ----------------------------------------------------------------------- */
static int parse_prefix(const char *s, uint32_t *dst, uint32_t *mask)
{
    /* format: a.b.c.d/prefix or a.b.c.d (no prefix = /32) */
    char buf[20]; int i = 0;
    while (s[i] && s[i] != '/' && i < 19) { buf[i] = s[i]; i++; }
    buf[i] = '\0';
    if (parse_ipv4(buf, dst) != 0) return -1;
    if (s[i] == '/') {
        int p = 0, digits = 0;
        const char *pp = s + i + 1;
        while (*pp >= '0' && *pp <= '9') {
            p = p * 10 + (*pp - '0');
            if (++digits > 2 || p > 32) return -1;     /* '/33' used to shift by a negative count */
            pp++;
        }
        if (digits == 0 || *pp != '\0') return -1;
        /* Addresses and masks are kept in wire (memory-byte) order everywhere in
         * the stack (IP4(255,255,255,0) == 0x00FFFFFF). The host-order value
         * ~0U << (32-p) put the ones in the wrong bytes: '192.168.5.0/24' became
         * destination 0.168.5.0 with mask 0.255.255.255 and matched nothing. */
        *mask = p == 0 ? 0 : htonl(~0U << (32 - p));
    } else {
        *mask = 0xFFFFFFFFU;
    }
    *dst &= *mask;
    return 0;
}

static int parse_int_arg(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

void cmd_route(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: route show | route add <dst/pfx> <gw> [metric] [table]\n");
        terminal_writestring("       route del <dst/pfx> [table]\n");
        return;
    }
    if (strcmp(args[1], "show") == 0 || strcmp(args[1], "list") == 0) {
        route_list();
    } else if (strcmp(args[1], "add") == 0) {
        if (argc < 4) { terminal_writestring("usage: route add <dst/pfx> <gw> [metric] [table]\n"); return; }
        uint32_t dst, mask;
        if (parse_prefix(args[2], &dst, &mask) != 0) { terminal_writestring("invalid address/prefix (a.b.c.d/0-32)\n"); return; }
        NEED_IP(gw, args[3]);
        int metric = (argc >= 5) ? parse_int_arg(args[4]) : 0;
        int table  = (argc >= 6) ? parse_int_arg(args[5]) : 0;
        if (route_add(dst, mask, gw, 0, metric, table) == 0)
            terminal_writestring("route added\n");
        else
            terminal_writestring("route add: failed (table full?)\n");
    } else if (strcmp(args[1], "del") == 0) {
        if (argc < 3) { terminal_writestring("usage: route del <dst/pfx> [table]\n"); return; }
        uint32_t dst, mask;
        if (parse_prefix(args[2], &dst, &mask) != 0) { terminal_writestring("invalid address/prefix (a.b.c.d/0-32)\n"); return; }
        int table = (argc >= 4) ? parse_int_arg(args[3]) : 0;
        if (route_del(dst, mask, table) == 0)
            terminal_writestring("route deleted\n");
        else
            terminal_writestring("route del: not found\n");
    } else {
        terminal_writestring("route: unknown subcommand\n");
    }
}

void cmd_policy(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: policy show | policy add <src/pfx> <dst/pfx> <table> <prio>\n");
        terminal_writestring("       policy del <prio>\n");
        return;
    }
    if (strcmp(args[1], "show") == 0 || strcmp(args[1], "list") == 0) {
        policy_list();
    } else if (strcmp(args[1], "add") == 0) {
        if (argc < 6) { terminal_writestring("usage: policy add <src/pfx> <dst/pfx> <table> <prio>\n"); return; }
        uint32_t s, sm, d, dm;
        if (parse_prefix(args[2], &s, &sm) != 0) { terminal_writestring("invalid address/prefix (a.b.c.d/0-32)\n"); return; }
        if (parse_prefix(args[3], &d, &dm) != 0) { terminal_writestring("invalid address/prefix (a.b.c.d/0-32)\n"); return; }
        int table = parse_int_arg(args[4]);
        int prio  = parse_int_arg(args[5]);
        if (policy_add(s, sm, d, dm, table, prio) == 0)
            terminal_writestring("policy rule added\n");
        else
            terminal_writestring("policy add: failed\n");
    } else if (strcmp(args[1], "del") == 0) {
        if (argc < 3) { terminal_writestring("usage: policy del <prio>\n"); return; }
        if (policy_del(parse_int_arg(args[2])) == 0)
            terminal_writestring("policy rule deleted\n");
        else
            terminal_writestring("policy del: not found\n");
    } else {
        terminal_writestring("policy: unknown subcommand\n");
    }
}

/* -----------------------------------------------------------------------
 * Firewall commands
 * fw rule add <proto> <src/pfx> <dst/pfx> [dport] <ACCEPT|DROP|REJECT>
 * fw rule del <index>
 * fw rule list
 * fw nat add SNAT|DNAT <match_ip/pfx> [port] <new_ip> [new_port]
 * fw nat del <index>
 * fw nat list
 * fw ct
 * ----------------------------------------------------------------------- */
void cmd_fw(int argc, char **args)
{
    if (argc < 3) {
        terminal_writestring("usage: fw rule add <proto> <src/pfx> <dst/pfx> [dport] <ACCEPT|DROP|REJECT>\n");
        terminal_writestring("       fw rule del <idx> | fw rule list\n");
        terminal_writestring("       fw nat add SNAT|DNAT <match/pfx> [port] <new_ip> [new_port]\n");
        terminal_writestring("       fw nat del <idx> | fw nat list\n");
        terminal_writestring("       fw ct\n");
        return;
    }
    if (strcmp(args[1], "ct") == 0) {
        ct_dump();
        return;
    }
    if (strcmp(args[1], "rule") == 0) {
        if (argc < 3) { terminal_writestring("fw rule: add|del|list\n"); return; }
        if (strcmp(args[2], "list") == 0) { fw_rule_list(); return; }
        if (strcmp(args[2], "del") == 0) {
            if (argc < 4) { terminal_writestring("fw rule del <idx>\n"); return; }
            fw_rule_del(parse_int_arg(args[3]));
            terminal_writestring("rule deleted\n"); return;
        }
        if (strcmp(args[2], "add") == 0) {
            /* fw rule add <proto> <src/pfx> <dst/pfx> [dport] <action> */
            if (argc < 7) {
                terminal_writestring("fw rule add <proto> <src/pfx> <dst/pfx> [dport] <ACCEPT|DROP|REJECT>\n");
                return;
            }
            uint8_t proto = (uint8_t)parse_int_arg(args[3]);
            uint32_t s, sm, d, dm;
            if (parse_prefix(args[4], &s, &sm) != 0) { terminal_writestring("invalid address/prefix (a.b.c.d/0-32)\n"); return; }
            if (parse_prefix(args[5], &d, &dm) != 0) { terminal_writestring("invalid address/prefix (a.b.c.d/0-32)\n"); return; }
            uint16_t dport = 0; int action_idx = 6;
            if (argc > 7) { dport = (uint16_t)parse_int_arg(args[6]); action_idx = 7; }
            uint8_t action = FW_ACCEPT;
            if (strcmp(args[action_idx], "DROP")   == 0) action = FW_DROP;
            if (strcmp(args[action_idx], "REJECT") == 0) action = FW_REJECT;
            int idx = fw_rule_add(proto, s, sm, d, dm, 0, dport, action);
            if (idx >= 0) { terminal_writestring("rule added at index "); }
            else terminal_writestring("fw rule add: failed\n");
            return;
        }
    }
    if (strcmp(args[1], "nat") == 0) {
        if (strcmp(args[2], "list") == 0) { nat_rule_list(); return; }
        if (strcmp(args[2], "del") == 0) {
            if (argc < 4) { terminal_writestring("fw nat del <idx>\n"); return; }
            nat_rule_del(parse_int_arg(args[3]));
            terminal_writestring("NAT rule deleted\n"); return;
        }
        if (strcmp(args[2], "add") == 0) {
            /* fw nat add SNAT|DNAT <match/pfx> [port] <new_ip> [new_port] */
            if (argc < 6) {
                terminal_writestring("fw nat add SNAT|DNAT <match/pfx> [port] <new_ip> [new_port]\n");
                return;
            }
            nat_type_t type = NAT_SNAT;
            if (strcmp(args[3], "DNAT") == 0) type = NAT_DNAT;
            uint32_t match, mmask;
            if (parse_prefix(args[4], &match, &mmask) != 0) { terminal_writestring("invalid address/prefix (a.b.c.d/0-32)\n"); return; }
            int next = 5;
            uint16_t mport = 0;
            /* heuristic: if next arg has no dots, treat as port */
            int has_dot = 0;
            for (const char *p = args[next]; *p; p++) if (*p == '.') { has_dot=1; break; }
            if (!has_dot && argc > next + 1) {
                if (parse_u16(args[next], &mport, 0) != 0) { terminal_writestring("invalid port (1-65535)\n"); return; }
                next++;
            }
            NEED_IP(new_ip, args[next]); next++;
            uint16_t new_port = 0;
            if (argc > next && parse_u16(args[next], &new_port, 0) != 0) { terminal_writestring("invalid port (1-65535)\n"); return; }
            int idx = nat_rule_add(type, match, mmask, mport, new_ip, new_port);
            if (idx >= 0) terminal_writestring("NAT rule added\n");
            else terminal_writestring("fw nat add: failed\n");
            return;
        }
    }
    terminal_writestring("fw: unknown subcommand\n");
}

/* -----------------------------------------------------------------------
 * Tunnel commands
 * gre add <local_ip> <remote_ip> [key]
 * gre del <idx>
 * gre list
 * gre send <idx> (sends a test ICMP-in-GRE — minimal demo)
 *
 * ipip add <local_ip> <remote_ip>
 * ipip del <idx>
 * ipip list
 *
 * wg add <remote_ip> <remote_port> <local_port> <psk_hex64> <peer_id_hex>
 * wg del <idx>
 * wg list
 * ----------------------------------------------------------------------- */
void cmd_gre(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: gre add <local> <remote> [key_hex] | gre del <idx> | gre list\n");
        return;
    }
    if (strcmp(args[1], "add") == 0) {
        if (argc < 4) { terminal_writestring("gre add <local> <remote> [key_hex]\n"); return; }
        NEED_IP(local, args[2]);
        NEED_IP(remote, args[3]);
        uint32_t key = 0; int use_key = 0;
        if (argc >= 5) {
            use_key = 1;
            for (const char *p = args[4]; *p; p++)
                key = key * 16 + (*p>='a'?*p-'a'+10:*p>='A'?*p-'A'+10:*p-'0');
        }
        int idx = gre_tunnel_add(local, remote, key, use_key);
        if (idx >= 0) { terminal_writestring("GRE tunnel added at index "); terminal_putchar('0'+idx); terminal_putchar('\n'); }
        else terminal_writestring("gre add: failed\n");
    } else if (strcmp(args[1], "del") == 0) {
        if (argc < 3) { terminal_writestring("gre del <idx>\n"); return; }
        gre_tunnel_del(parse_int_arg(args[2]));
        terminal_writestring("GRE tunnel deleted\n");
    } else if (strcmp(args[1], "list") == 0) {
        gre_tunnel_list();
    } else {
        terminal_writestring("gre: unknown subcommand\n");
    }
}

void cmd_ipip(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: ipip add <local> <remote> | ipip del <idx> | ipip list\n");
        return;
    }
    if (strcmp(args[1], "add") == 0) {
        if (argc < 4) { terminal_writestring("ipip add <local> <remote>\n"); return; }
        NEED_IP(ipip_local, args[2]);
        NEED_IP(ipip_remote, args[3]);
        int idx = ipip_tunnel_add(ipip_local, ipip_remote);
        if (idx >= 0) { terminal_writestring("IPIP tunnel added at index "); terminal_putchar('0'+idx); terminal_putchar('\n'); }
        else terminal_writestring("ipip add: failed\n");
    } else if (strcmp(args[1], "del") == 0) {
        if (argc < 3) { terminal_writestring("ipip del <idx>\n"); return; }
        ipip_tunnel_del(parse_int_arg(args[2]));
        terminal_writestring("IPIP tunnel deleted\n");
    } else if (strcmp(args[1], "list") == 0) {
        ipip_tunnel_list();
    } else {
        terminal_writestring("ipip: unknown subcommand\n");
    }
}

void cmd_wg(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: wg add <remote_ip> <rport> <lport> <psk64hex> <peer_id_hex>\n");
        terminal_writestring("       wg del <idx> | wg list\n");
        return;
    }
    if (strcmp(args[1], "add") == 0) {
        if (argc < 7) {
            terminal_writestring("wg add <remote_ip> <rport> <lport> <psk_hex64> <peer_id_hex>\n");
            return;
        }
        NEED_IP(rip, args[2]);
        NEED_PORT(rport, args[3]);
        NEED_PORT(lport, args[4]);
        /* Parse 64 hex chars = 32 bytes PSK */
        const char *phex = args[5];
        if (strlen(phex) != 64) { terminal_writestring("wg: PSK must be 64 hex chars\n"); return; }
        uint8_t psk[32];
        for (int i = 0; i < 32; i++) {
            int b = parse_hex_byte(phex + i*2);
            if (b < 0) { terminal_writestring("wg: invalid PSK hex\n"); return; }
            psk[i] = (uint8_t)b;
        }
        uint32_t peer_id = 0;
        for (const char *p = args[6]; *p; p++)
            peer_id = peer_id*16 + (*p>='a'?*p-'a'+10:*p>='A'?*p-'A'+10:*p-'0');
        int idx = wgtun_add(rip, rport, lport, psk, peer_id);
        if (idx >= 0) { terminal_writestring("WG tunnel added at index "); terminal_putchar('0'+idx); terminal_putchar('\n'); }
        else terminal_writestring("wg add: failed\n");
    } else if (strcmp(args[1], "del") == 0) {
        if (argc < 3) { terminal_writestring("wg del <idx>\n"); return; }
        wgtun_del(parse_int_arg(args[2]));
        terminal_writestring("WG tunnel deleted\n");
    } else if (strcmp(args[1], "list") == 0) {
        wgtun_list();
    } else {
        terminal_writestring("wg: unknown subcommand\n");
    }
}

/* ---- ws: a WebSocket client, so live services can be talked to from the shell ---- */

void cmd_ws(int argc, char **args)
{
    if (argc < 2) {
        terminal_writestring("usage: ws <ws://host[:port]/path | wss://...> [message]\n");
        terminal_writestring("  Connects, sends the message if one is given, then prints what\n");
        terminal_writestring("  arrives until the server closes or 10 seconds pass quietly.\n");
        return;
    }
    const char *url = args[1];
    int use_tls = 0;
    if (strncmp(url, "wss://", 6) == 0) { use_tls = 1; url += 6; }
    else if (strncmp(url, "ws://", 5) == 0) { url += 5; }
    else { terminal_writestring("ws: only ws:// and wss:// are supported\n"); return; }

    char host[256], path[256];
    int i = 0;
    while (*url && *url != '/' && *url != ':' && i < 255) host[i++] = *url++;
    host[i] = 0;
    uint16_t port = use_tls ? 443 : 80;
    if (*url == ':') {
        url++;
        uint32_t pv = 0; int pd = 0;
        while (*url >= '0' && *url <= '9') { pv = pv * 10 + (uint32_t)(*url - '0'); url++; if (++pd > 5) break; }
        if (pd == 0 || pv == 0 || pv > 65535) { terminal_writestring("ws: invalid port\n"); return; }
        port = (uint16_t)pv;
    }
    if (*url == '/') { int j = 0; while (*url && j < 255) path[j++] = *url++; path[j] = 0; }
    else { path[0] = '/'; path[1] = 0; }

    uint32_t ip;
    int is_ip = 1;
    for (const char *p = host; *p; p++) if ((*p < '0' || *p > '9') && *p != '.') { is_ip = 0; break; }
    if (is_ip) {
        if (parse_ipv4(host, &ip) != 0) { terminal_writestring("ws: invalid IP address\n"); return; }
    } else {
        terminal_writestring("Resolving... ");
        int rc = dns_resolve(host, &ip);
        if (rc != 0) { terminal_writestring("FAILED\n"); return; }
        terminal_writestring("OK\n");
    }

    ws_t *ws = (ws_t *)malloc(sizeof(ws_t));
    if (!ws) { terminal_writestring("ws: out of memory\n"); return; }
    terminal_writestring("Connecting... ");
    if (ws_connect(ws, ip, host, port, path, use_tls) != 0) {
        terminal_writestring("FAILED (see dmesg)\n");
        free(ws);
        return;
    }
    terminal_writestring("connected\n");

    if (argc >= 3) {
        if (ws_send_text(ws, args[2]) != 0) terminal_writestring("ws: send failed\n");
        else { terminal_writestring("> "); terminal_writestring(args[2]); terminal_putchar('\n'); }
    }

    uint8_t *buf = (uint8_t *)malloc(4096);
    if (buf) {
        for (int msgs = 0; msgs < 100; msgs++) {
            int op = 0;
            int n = ws_recv(ws, buf, 4095, &op, 10000);
            if (n <= 0) break;
            buf[n] = 0;
            terminal_writestring("< ");
            if (op == WS_TEXT) terminal_writestring((const char *)buf);
            else { terminal_writestring("["); print_num(n); terminal_writestring(" binary bytes]"); }
            terminal_putchar('\n');
        }
        free(buf);
    }
    ws_close(ws);
    free(ws);
    terminal_writestring("closed\n");
}
