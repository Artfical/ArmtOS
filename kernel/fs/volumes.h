#ifndef VOLUMES_H
#define VOLUMES_H

#include <stdint.h>

/* Removable disks: mounted at /Volumes/<label> when plugged in, ejected before unplugging. */

#define VOLUMES_MAX 4

enum { VOLUMES_NONE, VOLUMES_ADDED, VOLUMES_REMOVED, VOLUMES_UNSAFE };

typedef struct {
    char name[32];          /* the volume label */
    char path[64];          /* /Volumes/<name> */
    uint64_t bytes;
} volume_info_t;

/* looks for disks plugged in or pulled out; returns what happened (VOLUMES_UNSAFE: a mounted
 * disk was pulled out without being ejected), with the disk's name in volumes_last_name() */
int  volumes_poll(void);
const char *volumes_last_name(void);
int  volumes_list(volume_info_t *out, int max);
int  volumes_eject(const char *path);       /* 0 = safe to unplug; -1 = in use or unknown */

#endif
