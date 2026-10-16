#ifndef UIHOST_H
#define UIHOST_H

#include <stdint.h>

/*
 * What a desktop gives its native apps (uiapp.h). tOS implements it in wm_gfx.c over its VFS,
 * scheduler and network stack; ArmtOS in desktop.c over the SD card's FAT partition. Anything a
 * platform cannot do returns -1 / 0 and the app says so.
 */

typedef struct { char name[64]; uint32_t size; int is_dir; } uihost_entry_t;
typedef struct { uint32_t pid; char name[32]; int state; uint32_t cpu; } uihost_task_t;
typedef struct {
    int up;
    uint32_t ip, gateway, dns, netmask;
    uint8_t mac[6];
    char driver[32];
    uint32_t rx_bytes, tx_bytes, rx_packets, tx_packets;
} uihost_net_t;

uint32_t uihost_ms(void);
void uihost_time(int *year, int *month, int *day, int *hour, int *min, int *sec);

/* files: paths are absolute ("/" is the root the platform has) */
const char *uihost_home(void);
int  uihost_list(const char *dir, uihost_entry_t *out, int max);    /* count, or -1 */
int  uihost_read(const char *path, uint8_t **data, uint32_t *len);  /* malloc'd; 0 = ok */
int  uihost_write(const char *path, const void *data, uint32_t len);
int  uihost_mkdir(const char *path);
int  uihost_delete(const char *path);
int  uihost_rename(const char *from, const char *to);

/* other apps and the desktop itself */
void uihost_open(const char *app_id, const char *path);
int  uihost_set_wallpaper(const char *path);         /* PNG or JPEG; 0 = ok */

/* the system */
int  uihost_tasks(uihost_task_t *out, int max);
int  uihost_kill(uint32_t pid);
void uihost_memory(uint32_t *total_kb, uint32_t *used_kb);
void uihost_net(uihost_net_t *n);
int  uihost_net_start(void);
const char *uihost_system_name(void);

#endif
