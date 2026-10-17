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
const char *uihost_hostname(void);

/* desktop settings: "dark", "dock_magnify", "mouse_speed", "wallpaper" (0-3), "width", "height";
 * set also takes "resolution" (an index into 1024x768, 1280x720, 1366x768, 1600x900, 1920x1080) */
int  uihost_get(const char *key);
void uihost_set(const char *key, int value);

/* accounts */
typedef struct { char name[32]; char full[48]; uint32_t uid; int admin; int current; } uihost_user_t;
int  uihost_users(uihost_user_t *out, int max);
int  uihost_add_user(const char *name, const char *password, const char *full, int admin);   /* needs authorisation */
int  uihost_set_password(const char *name, const char *password);                          /* needs authorisation */

/* Asks the person at the desktop for their password (the password dialog), without blocking:
 * poll uihost_authorized() -- 0 while asking, 1 granted, -1 refused or cancelled. A grant lasts
 * for one privileged call. */
void uihost_authorize(const char *reason);
int  uihost_authorized(void);


/* Notifications: a banner at the top right that stays in Notification Centre. app is the app's
 * id (its icon is used) or a name such as "System"; icon is an image name ("doc48_disk",
 * "icon48_paint") or NULL; act1/act2 are button labels or NULL (with buttons the banner waits
 * for an answer). Returns an id for uihost_notify_result: 0 waiting, 1 or 2 that button,
 * 3 the notification itself was clicked, -1 closed, -2 unknown. Do Not Disturb ("dnd" in
 * uihost_get/uihost_set) keeps banners away but the Centre still collects. */
int  uihost_notify(const char *app, const char *icon, const char *title, const char *body, const char *act1, const char *act2);
int  uihost_notify_result(int id);


/* Removable disks: what is mounted (under /Volumes) and ejecting one (0 = safe to unplug,
 * -1 = in use). Unplugging a disk without ejecting it is reported by the desktop itself. */
typedef struct { char name[32]; char path[64]; uint64_t bytes; } uihost_volume_t;
int  uihost_volumes(uihost_volume_t *out, int max);
int  uihost_eject(const char *path);

#endif
