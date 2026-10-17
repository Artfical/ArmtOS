#include "volumes.h"
#include "blockdev.h"
#include "diskops.h"
#include "vfs.h"
#include "auth.h"
#include "uhci.h"
#include "usb_msd.h"
#include "string.h"
#include "klog.h"
#include "ramfs.h"

/*
 * Removable disks, the way a Mac treats them: a USB disk that is plugged in is mounted at
 * /Volumes/<its label> by itself and shows up in the Finder; ejecting unmounts it so it can
 * be pulled out safely; pulling it out without ejecting is noticed and reported.
 *
 * Hot-plugging is found by polling the UHCI root ports' connect bits (the controller raises no
 * interrupt here); volumes_poll() is cheap and the desktop calls it twice a second.
 */

static uhci_controller_t uhci_ctrl;      /* uhci_init hands every caller the one controller */

typedef struct {
    int used;
    int state;                  /* VOL_MOUNTED, or VOL_EJECTED: still plugged in, unmounted */
    char name[32];
    char path[64];
    char bd[16];
    uint64_t bytes;
    int port;                   /* the root port it is on */
} vol_t;

enum { VOL_MOUNTED = 1, VOL_EJECTED = 2 };

static vol_t vols[VOLUMES_MAX];
static usb_msd_device_t usb_dev;
static int have_uhci = -1;      /* -1 not looked yet */
static int port_seen[2];
static char last_name[32];
static int started;

static void trim_label(const uint8_t *p, int n, char *out)
{
    int l = n;
    while (l > 0 && (p[l - 1] == ' ' || p[l - 1] == 0)) l--;
    int o = 0;
    for (int i = 0; i < l && o < 31; i++) {
        char c = (char)p[i];
        if (c == '/' || c < 32 || c > 126) c = '_';
        out[o++] = c;
    }
    out[o] = 0;
}

/* the volume label from the boot sector (FAT) or superblock (ext), "NO NAME" otherwise */
static void read_label(blockdev_t *bd, const char *fs, char *out)
{
    static uint8_t sec[1024];
    out[0] = 0;
    if (!strcmp(fs, "fat32") || !strcmp(fs, "fat16")) {
        if (blockdev_read(bd, 0, 1, sec) == 0) trim_label(sec + (!strcmp(fs, "fat32") ? 71 : 43), 11, out);
    } else if (!strncmp(fs, "ext", 3)) {
        if (blockdev_read_bytes(bd, 1024 + 120, 16, sec) == 0) trim_label(sec, 16, out);
    }
    if (!out[0] || !strcmp(out, "NO NAME")) strcpy(out, "NO NAME");
}

static int name_taken(const char *n)
{
    for (int i = 0; i < VOLUMES_MAX; i++) if (vols[i].used && !strcmp(vols[i].name, n)) return 1;
    return 0;
}

static vol_t *mount_dev(blockdev_t *bd, int port)
{
    vol_t *v = 0;
    for (int i = 0; i < VOLUMES_MAX && !v; i++) if (!vols[i].used) v = &vols[i];
    if (!v) return 0;
    const char *fs = diskops_detect(bd->name);
    if (!fs) { klog_write("volumes: no file system I know on the disk\n"); return 0; }
    char label[32];
    read_label(bd, fs, label);
    /* two disks with one name: the second is "NAME 1", like macOS does */
    char name[32];
    strcpy(name, label);
    for (int k = 1; name_taken(name) && k < 10; k++) {
        strcpy(name, label);
        int l = (int)strlen(name);
        if (l > 28) l = 28;
        name[l] = ' '; name[l + 1] = (char)('0' + k); name[l + 2] = 0;
    }
    char path[64] = "/Volumes/";
    strcat(path, name);

    auth_saved_t saved;
    auth_become(0, 0, &saved);
    if (!vfs_exists("/Volumes")) vfs_mkdir("/Volumes", 0755);
    if (!vfs_exists(path)) vfs_mkdir(path, 0755);
    char err[96];
    int rc = diskops_mount(bd->name, path, fs, err, sizeof(err));
    auth_restore(&saved);
    if (rc != 0) { klog_write("volumes: mount failed: "); klog_write(err); klog_write("\n"); return 0; }

    memset(v, 0, sizeof(*v));
    v->used = 1;
    v->state = VOL_MOUNTED;
    strcpy(v->name, name);
    strcpy(v->path, path);
    strcpy(v->bd, bd->name);
    v->bytes = bd->total_sectors * bd->sector_size;
    v->port = port;
    klog_write("volumes: mounted ");
    klog_write(path);
    klog_write("\n");
    return v;
}

static void forget(vol_t *v, int unplugged)
{
    blockdev_t *bd = blockdev_find(v->bd);
    if (v->state == VOL_MOUNTED) {
        auth_saved_t saved;
        auth_become(0, 0, &saved);
        char err[96];
        if (diskops_umount(v->path, err, sizeof(err)) != 0) {
            /* files were open on it: the disk is gone all the same */
            vfs_unmount_force(v->path);
            if (bd) { bd->mounted = 0; bd->fs_ctx = 0; }
        }
        ramfs_rmdir(v->path);
        auth_restore(&saved);
    }
    if (unplugged && bd) {
        usb_dev.present = 0;
        blockdev_unregister(bd);
    }
    v->used = 0;
}

static vol_t *on_port(int port)
{
    for (int i = 0; i < VOLUMES_MAX; i++) if (vols[i].used && vols[i].port == port) return &vols[i];
    return 0;
}

static void start(void)
{
    started = 1;
    have_uhci = uhci_init(&uhci_ctrl) == 0;
    /* a disk found while booting is already a block device: mount it */
    for (int i = 0; i < blockdev_count(); i++) {
        blockdev_t *bd = blockdev_get(i);
        if (!bd || bd->type != BLOCKDEV_USB_MSD || bd->mounted) continue;
        int port = 0;
        if (have_uhci) port = uhci_port_connected(&uhci_ctrl, 0) ? 0 : 1;
        if (mount_dev(bd, port)) port_seen[port] = 1;
    }
    if (have_uhci)
        for (int p = 0; p < 2; p++) if (uhci_port_connected(&uhci_ctrl, p)) port_seen[p] = 1;
}

int volumes_poll(void)
{
    if (!started) start();
    if (!have_uhci) return VOLUMES_NONE;
    for (int p = 0; p < 2; p++) {
        int now = uhci_port_connected(&uhci_ctrl, p) ? 1 : 0;
        if (now == port_seen[p]) continue;
        port_seen[p] = now;
        if (now) {
            /* plugged in: let it power up, then enumerate it and mount it */
            for (volatile int d = 0; d < 2000000; d++);
            if (usb_dev.present) continue;            /* one USB disk at a time */
            if (usb_msd_init(&usb_dev) != 0) { klog_write("volumes: a USB device that is not a disk\n"); continue; }
            blockdev_register_usb(&usb_dev);
            blockdev_t *bd = 0;
            for (int i = 0; i < blockdev_count(); i++) {
                blockdev_t *b = blockdev_get(i);
                if (b && b->type == BLOCKDEV_USB_MSD && b->driver_data == &usb_dev) bd = b;
            }
            if (bd && mount_dev(bd, p)) { strcpy(last_name, on_port(p)->name); return VOLUMES_ADDED; }
        } else {
            vol_t *v = on_port(p);
            if (!v) continue;
            int was_mounted = v->state == VOL_MOUNTED;
            strcpy(last_name, v->name);
            forget(v, 1);
            if (was_mounted) {
                klog_write("volumes: disk pulled out without ejecting\n");
                return VOLUMES_UNSAFE;
            }
            return VOLUMES_REMOVED;
        }
    }
    return VOLUMES_NONE;
}

const char *volumes_last_name(void) { return last_name; }

int volumes_list(volume_info_t *out, int max)
{
    if (!started) start();
    int n = 0;
    for (int i = 0; i < VOLUMES_MAX && n < max; i++) {
        if (!vols[i].used || vols[i].state != VOL_MOUNTED) continue;
        strcpy(out[n].name, vols[i].name);
        strcpy(out[n].path, vols[i].path);
        out[n].bytes = vols[i].bytes;
        n++;
    }
    return n;
}

int volumes_eject(const char *path)
{
    for (int i = 0; i < VOLUMES_MAX; i++) {
        vol_t *v = &vols[i];
        if (!v->used || v->state != VOL_MOUNTED || strcmp(v->path, path)) continue;
        blockdev_t *bd = blockdev_find(v->bd);
        auth_saved_t saved;
        auth_become(0, 0, &saved);
        char err[96];
        if (bd) blockdev_flush(bd);
        int rc = diskops_umount(v->path, err, sizeof(err));
        if (rc == 0) ramfs_rmdir(v->path);
        auth_restore(&saved);
        if (rc != 0) return -1;                  /* in use: a file is open on it */
        strcpy(last_name, v->name);
        v->state = VOL_EJECTED;                  /* safe to pull out now; it is forgotten then */
        return 0;
    }
    return -1;
}
