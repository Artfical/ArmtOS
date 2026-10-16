#include "uihost.h"
#include "wm.h"
#include "wm_internal.h"
#include "fsbridge.h"
#include "scheduler.h"
#include "memory.h"
#include "string.h"
#include "cmos.h"
#include "debugmon.h"
#include "auth.h"
#include "net.h"
#include "nic.h"

/* uihost.h for tOS: the VFS, the scheduler and the network stack. */

uint32_t uihost_ms(void) { return debugmon_uptime_ms(); }

void uihost_time(int *year, int *month, int *day, int *hour, int *min, int *sec)
{
    cmos_time_t t;
    cmos_get_time(&t);
    *year = t.year; *month = t.month; *day = t.day;
    *hour = t.hour; *min = t.minute; *sec = t.second;
}

const char *uihost_home(void)
{
    static char home[64];
    uint32_t uid, gid;
    auth_session(&uid, &gid);
    auth_user_t u;
    if (auth_lookup_uid(uid, &u) == 0 && u.home[0] && fsbridge_is_dir(u.home)) strcpy(home, u.home);
    else strcpy(home, "/");
    return home;
}

int uihost_list(const char *dir, uihost_entry_t *out, int max)
{
    static vfs_entry_t ents[128];
    int n = fsbridge_list(dir, ents, max < 128 ? max : 128);
    if (n < 0) return -1;
    int o = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(ents[i].name, ".") || !strcmp(ents[i].name, "..")) continue;
        strncpy(out[o].name, ents[i].name, sizeof(out[o].name) - 1);
        out[o].name[sizeof(out[o].name) - 1] = 0;
        out[o].size = ents[i].size;
        out[o].is_dir = ents[i].is_dir;
        o++;
    }
    return o;
}

int uihost_read(const char *path, uint8_t **data, uint32_t *len)
{
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) return -1;
    uint32_t n = fsbridge_size(path);
    uint8_t *b = malloc(n + 1);
    if (!b) return -1;
    int got = n ? fsbridge_read(path, b, n, 0) : 0;
    if (got < 0) { free(b); return -1; }
    b[got] = 0;
    *data = b;
    *len = (uint32_t)got;
    return 0;
}

int uihost_write(const char *path, const void *data, uint32_t len)
{
    /* replace, not overwrite in place: a shorter file must not keep the old tail */
    if (fsbridge_exists(path)) fsbridge_delete(path);
    if (fsbridge_create(path) != 0 && !fsbridge_exists(path)) return -1;
    if (!len) return 0;
    return fsbridge_write(path, data, len, 0) == (int)len ? 0 : -1;
}

int uihost_mkdir(const char *path) { return fsbridge_mkdir(path); }
int uihost_delete(const char *path) { return fsbridge_delete(path); }
int uihost_rename(const char *from, const char *to) { return fsbridge_rename(from, to); }

void uihost_open(const char *app_id, const char *path)
{
    if (wm_gfx_open_native(app_id, path) == 0) return;
    /* the apps that are still the classic character-cell ones */
    if (!strcmp(app_id, "pdf") && path) wm_open_pdfviewer_file(path);
    else if (!strcmp(app_id, "run") && path) {
        char cmd[64] = "run ";
        int o = 4;
        for (const char *q = path; *q && o < (int)sizeof(cmd) - 1; q++) cmd[o++] = *q;
        cmd[o] = 0;
        wm_open_terminal(cmd);
    } else if (!strcmp(app_id, "music")) wm_open_app("mediaplayer");
    else wm_open_app(app_id);
}

static uihost_task_t *t_out;
static int t_n, t_max;

static void task_cb(uint32_t pid, const char *name, uint32_t state)
{
    if (t_n >= t_max) return;
    t_out[t_n].pid = pid;
    strncpy(t_out[t_n].name, name, sizeof(t_out[t_n].name) - 1);
    t_out[t_n].name[sizeof(t_out[t_n].name) - 1] = 0;
    t_out[t_n].state = (int)state;
    t_out[t_n].cpu = task_get_cpu_ticks(pid);
    t_n++;
}

int uihost_tasks(uihost_task_t *out, int max)
{
    t_out = out; t_n = 0; t_max = max;
    task_foreach(task_cb);
    return t_n;
}

int uihost_kill(uint32_t pid)
{
    if (wm_kill_task_window(pid) == 0) return 0;
    return task_kill(pid);
}

void uihost_memory(uint32_t *total_kb, uint32_t *used_kb) { memory_get_usage(total_kb, used_kb); }

void uihost_net(uihost_net_t *n)
{
    memset(n, 0, sizeof(*n));
    n->up = nic_send != 0;
    n->ip = net_ip; n->gateway = net_gateway; n->dns = net_dns; n->netmask = net_netmask;
    memcpy(n->mac, net_mac, 6);
    strncpy(n->driver, nic_driver_name, sizeof(n->driver) - 1);
    n->rx_bytes = nic_rx_bytes; n->tx_bytes = nic_tx_bytes;
    n->rx_packets = nic_rx_packets; n->tx_packets = nic_tx_packets;
}

int uihost_net_start(void) { return nic_send ? 0 : -1; }    /* tOS brings the network up at boot */

const char *uihost_system_name(void) { return "tOS"; }
