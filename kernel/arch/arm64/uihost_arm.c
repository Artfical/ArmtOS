#include <stdint.h>
#include "irq.h"
#include "heap.h"
#include "fat.h"
#include "emmc.h"
#include "string_arm.h"
#include "compat/cmos.h"
#include "../../display/uihost.h"

/*
 * uihost.h for ArmtOS. Files are the SD card's FAT boot partition, whose root is all there is
 * (8.3 names; no folders are made, renamed or removed). There is one console user and no
 * account database, so an authorisation is simply granted.
 */

int  netcmd_up(void);
int  netcmd_is_up(void);
extern uint32_t net_ip, net_gateway, net_dns, net_netmask;
extern uint8_t net_mac[6];
extern char nic_driver_name[32];
extern uint32_t nic_rx_bytes, nic_tx_bytes, nic_rx_packets, nic_tx_packets;

/* desktop.c */
void desktop_open(const char *id, const char *arg);
int  desktop_set_wallpaper_file(const char *path);
int  desktop_get(const char *key);
void desktop_set(const char *key, int value);
int  desktop_notify(const char *app, const char *icon, const char *title, const char *body, const char *act1, const char *act2);
int  notify_result(int id);

uint32_t uihost_ms(void) { return (uint32_t)(timer_us() / 1000); }

void uihost_time(int *year, int *month, int *day, int *hour, int *min, int *sec)
{
    cmos_time_t t;
    cmos_get_time(&t);
    *year = t.year; *month = t.month; *day = t.day; *hour = t.hour; *min = t.minute; *sec = t.second;
}

const char *uihost_home(void) { return "/"; }

static uihost_entry_t *l_out;
static int l_n, l_max;

static void list_cb(const char *name, uint32_t size)
{
    if (l_n >= l_max) return;
    strncpy(l_out[l_n].name, name, sizeof(l_out[l_n].name) - 1);
    l_out[l_n].name[sizeof(l_out[l_n].name) - 1] = 0;
    l_out[l_n].size = size;
    l_out[l_n].is_dir = 0;
    l_n++;
}

static int card(void) { return emmc_init() == 0 && fat_mount() == 0 ? 0 : -1; }

int uihost_list(const char *dir, uihost_entry_t *out, int max)
{
    if (strcmp(dir, "/") || card() != 0) return -1;
    l_out = out; l_n = 0; l_max = max;
    fat_list(list_cb);
    return l_n;
}

static const char *root_name(const char *path)
{
    if (path[0] != '/') return path;
    for (const char *p = path + 1; *p; p++) if (*p == '/') return 0;      /* only the root exists */
    return path + 1;
}

int uihost_read(const char *path, uint8_t **data, uint32_t *len)
{
    const char *name = root_name(path);
    if (!name || card() != 0) return -1;
    /* the size first, from the directory */
    static uihost_entry_t ents[64];
    int n = uihost_list("/", ents, 64);
    uint32_t size = 0;
    int found = 0;
    for (int i = 0; i < n; i++) {
        const char *a = ents[i].name, *b = name;
        int same = 1;
        for (; *a || *b; a++, b++) {
            char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
            if (x != y) { same = 0; break; }
        }
        if (same) { size = ents[i].size; found = 1; break; }
    }
    if (!found) return -1;
    uint8_t *b = malloc(size + 1);
    if (!b) return -1;
    int got = size ? fat_read_file(name, b, size) : 0;
    if (got < 0) { free(b); return -1; }
    b[got] = 0;
    *data = b;
    *len = (uint32_t)got;
    return 0;
}

int uihost_write(const char *path, const void *data, uint32_t len)
{
    const char *name = root_name(path);
    if (!name || card() != 0) return -1;
    return fat_write_file(name, (const uint8_t *)data, len) < 0 ? -1 : 0;
}

int uihost_mkdir(const char *path) { (void)path; return -1; }
int uihost_delete(const char *path) { (void)path; return -1; }
int uihost_rename(const char *from, const char *to) { (void)from; (void)to; return -1; }

void uihost_open(const char *app_id, const char *path) { desktop_open(app_id, path); }
int  uihost_set_wallpaper(const char *path) { return desktop_set_wallpaper_file(path); }

int  uihost_tasks(uihost_task_t *out, int max)
{
    /* one program at a time and no scheduler: the shell is the one task */
    if (max < 1) return 0;
    out[0].pid = 1;
    strcpy(out[0].name, "shell");
    out[0].state = 1;
    out[0].cpu = (uint32_t)(timer_us() / 10000);
    return 1;
}
int  uihost_kill(uint32_t pid) { (void)pid; return -1; }

void uihost_memory(uint32_t *total_kb, uint32_t *used_kb)
{
    *total_kb = (uint32_t)(heap_total() / 1024);
    *used_kb = (uint32_t)(heap_used() / 1024);
}

void uihost_net(uihost_net_t *n)
{
    memset(n, 0, sizeof(*n));
    n->up = netcmd_is_up();
    n->ip = net_ip; n->gateway = net_gateway; n->dns = net_dns; n->netmask = net_netmask;
    memcpy(n->mac, net_mac, 6);
    strncpy(n->driver, nic_driver_name, sizeof(n->driver) - 1);
    n->rx_bytes = nic_rx_bytes; n->tx_bytes = nic_tx_bytes;
    n->rx_packets = nic_rx_packets; n->tx_packets = nic_tx_packets;
}

int  uihost_net_start(void) { return netcmd_up(); }
const char *uihost_system_name(void) { return "ArmtOS"; }
const char *uihost_hostname(void) { return "raspberrypi"; }

int  uihost_get(const char *key) { return desktop_get(key); }
void uihost_set(const char *key, int value) { desktop_set(key, value); }

int uihost_users(uihost_user_t *out, int max)
{
    if (max < 1) return 0;
    strcpy(out[0].name, "pi");
    strcpy(out[0].full, "Raspberry Pi");
    out[0].uid = 0;
    out[0].admin = 1;
    out[0].current = 1;
    return 1;
}

int uihost_add_user(const char *name, const char *password, const char *full, int admin)
{
    (void)name; (void)password; (void)full; (void)admin;
    return -1;                              /* no account database on the board */
}
int uihost_set_password(const char *name, const char *password) { (void)name; (void)password; return -1; }
void uihost_authorize(const char *reason) { (void)reason; }
int  uihost_authorized(void) { return 1; }

int uihost_notify(const char *app, const char *icon, const char *title, const char *body, const char *act1, const char *act2)
{
    return desktop_notify(app, icon, title, body, act1, act2);
}
int uihost_notify_result(int id) { return notify_result(id); }

/* the SD card is the only disk and it is not removable while the system runs from it */
int uihost_volumes(uihost_volume_t *out, int max) { (void)out; (void)max; return 0; }
int uihost_eject(const char *path) { (void)path; return -1; }
