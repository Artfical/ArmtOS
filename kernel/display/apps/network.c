#include "../uiapp.h"
#include "../uihost.h"
#include "string.h"
#include "memory.h"

/* Network: the adapter, the addresses, and live download/upload speed with a graph. */

#define HIST 90

typedef struct {
    uint32_t last_ms, last_rx, last_tx;
    uint32_t rx_rate, tx_rate;          /* bytes per second */
    uint32_t rx_hist[HIST], tx_hist[HIST];
    int n;
    char msg[64];
} net_t;

static void put_ip(char *o, uint32_t ip)
{
    for (int i = 0; i < 4; i++) { ui_num(o, (ip >> (8 * i)) & 255); if (i < 3) strcat(o, "."); }
}

static void rate(char *o, uint32_t bps)
{
    if (bps < 1024) { ui_num(o, bps); strcat(o, " B/s"); }
    else if (bps < 1024 * 1024) { ui_num(o, bps / 1024); strcat(o, "."); ui_num(o, bps % 1024 * 10 / 1024); strcat(o, " KB/s"); }
    else { ui_num(o, bps / (1024 * 1024)); strcat(o, "."); ui_num(o, bps % (1024 * 1024) * 10 / (1024 * 1024)); strcat(o, " MB/s"); }
}

static void sample(net_t *n)
{
    uihost_net_t s;
    uihost_net(&s);
    uint32_t now = uihost_ms(), dt = now - n->last_ms;
    if (n->last_ms && dt) {
        n->rx_rate = (uint32_t)((uint64_t)(s.rx_bytes - n->last_rx) * 1000 / dt);
        n->tx_rate = (uint32_t)((uint64_t)(s.tx_bytes - n->last_tx) * 1000 / dt);
        if (n->n == HIST) {
            memmove(n->rx_hist, n->rx_hist + 1, (HIST - 1) * 4);
            memmove(n->tx_hist, n->tx_hist + 1, (HIST - 1) * 4);
            n->n--;
        }
        n->rx_hist[n->n] = n->rx_rate;
        n->tx_hist[n->n] = n->tx_rate;
        n->n++;
    }
    n->last_ms = now; n->last_rx = s.rx_bytes; n->last_tx = s.tx_bytes;
}

static void net_init(uiapp_t *a, const char *arg) { (void)arg; sample(a->st); }

static int net_tick(uiapp_t *a)
{
    net_t *n = a->st;
    if (uihost_ms() - n->last_ms >= 1000) { sample(n); return 1; }
    return 0;
}

static void kv(gfx_surface_t *s, int x, int y, const char *k, const char *v)
{
    gfx_text(s, &ui_font_regular, x, y, k, UI_DIM, 255);
    gfx_text(s, &ui_font_medium, x + 110, y, v, UI_TEXT, 255);
}

static void net_draw(uiapp_t *a, gfx_surface_t *s, int x, int y, int w, int h)
{
    net_t *n = a->st;
    uihost_net_t st;
    uihost_net(&st);
    int connected = st.up && st.ip;

    /* the status card */
    gfx_rrect(s, x + 14, y + 14, w - 28, 76, 12, UI_CARD, 255);
    gfx_circle(s, x + 50, y + 52, 20, connected ? 0x14532D : 0x3F3F46, 255);
    gfx_blit(s, gfx_image("glyph_ethernet-port"), x + 42, y + 44, 255);
    gfx_text(s, &ui_font_title, x + 84, y + 24, connected ? "Connected" : st.up ? "No address" : "Not connected", UI_TEXT, 255);
    gfx_text(s, &ui_font_regular, x + 84, y + 56, st.up ? (st.driver[0] ? st.driver : "Ethernet") : "No network adapter is running", UI_DIM, 255);

    /* speeds */
    int cw = (w - 42) / 2;
    for (int k = 0; k < 2; k++) {
        int cx = x + 14 + k * (cw + 14);
        gfx_rrect(s, cx, y + 102, cw, 64, 12, UI_CARD, 255);
        gfx_text(s, &ui_font_small, cx + 14, y + 112, k ? "UPLOAD" : "DOWNLOAD", UI_DIM, 255);
        char r[24] = "";
        rate(r, k ? n->tx_rate : n->rx_rate);
        gfx_text(s, &ui_font_title, cx + 14, y + 130, r, k ? UI_ORANGE : UI_ACCENT, 255);
    }

    /* the graph */
    int gx = x + 14, gy = y + 178, gw = w - 28, gh = 120;
    gfx_rrect(s, gx, gy, gw, gh, 12, UI_CARD, 255);
    uint32_t peak = 1024;
    for (int i = 0; i < n->n; i++) { if (n->rx_hist[i] > peak) peak = n->rx_hist[i]; if (n->tx_hist[i] > peak) peak = n->tx_hist[i]; }
    for (int i = 0; i < n->n; i++) {
        int bx = gx + 12 + (gw - 24) - (n->n - i) * (gw - 24) / HIST;
        int bw = (gw - 24) / HIST;
        int rh = (int)((uint64_t)n->rx_hist[i] * (gh - 24) / peak), th = (int)((uint64_t)n->tx_hist[i] * (gh - 24) / peak);
        gfx_fill_alpha(s, bx, gy + gh - 12 - rh, bw, rh, UI_ACCENT, 150);
        gfx_fill_alpha(s, bx, gy + gh - 12 - th, bw, th, UI_ORANGE, 150);
    }
    char pk[24] = "peak ";
    rate(pk, peak);
    gfx_text(s, &ui_font_small, gx + gw - 12 - gfx_text_width(&ui_font_small, pk), gy + 8, pk, UI_DIM, 255);

    /* details */
    int dy = gy + gh + 16;
    char v[40];
    v[0] = 0; put_ip(v, st.ip);        kv(s, x + 24, dy, "Address", st.up ? v : "-");
    v[0] = 0; put_ip(v, st.gateway);   kv(s, x + 24, dy + 24, "Router", st.up ? v : "-");
    v[0] = 0; put_ip(v, st.dns);       kv(s, x + 24, dy + 48, "DNS", st.up ? v : "-");
    v[0] = 0;
    for (int i = 0; i < 6; i++) { char c[4] = { "0123456789abcdef"[st.mac[i] >> 4], "0123456789abcdef"[st.mac[i] & 15], i < 5 ? ':' : 0, 0 }; strcat(v, c); }
    kv(s, x + w / 2 + 10, dy, "MAC", st.up ? v : "-");
    v[0] = 0; ui_num(v, st.rx_packets); strcat(v, " in, "); ui_num(v, st.tx_packets); strcat(v, " out");
    kv(s, x + w / 2 + 10, dy + 24, "Packets", v);
    v[0] = 0; ui_size(v, st.rx_bytes); strcat(v, " / "); ui_size(v, st.tx_bytes);
    kv(s, x + w / 2 + 10, dy + 48, "Total", v);
    if (!st.up) ui_button(s, x + w - 170, y + 36, 150, 32, "Connect", UI_ACCENT, 0xFFFFFF, 0);
    (void)h;
}

static void net_press(uiapp_t *a, int px, int py, int b)
{
    (void)b;
    uihost_net_t st;
    uihost_net(&st);
    if (!st.up && ui_hit(px, py, a->w - 170, 36, 150, 32)) uihost_net_start();
    a->dirty = 1;
}

const uiapp_class_t app_network = {
    "netmon", "Network", "netmon", 560, 420, 0, sizeof(net_t),
    net_init, net_draw, net_press, 0, 0, 0, 0, net_tick, 0,
};
