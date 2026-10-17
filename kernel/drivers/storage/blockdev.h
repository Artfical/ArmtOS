#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include <stdint.h>
#include "ata.h"
#include "ahci.h"
#include "nvme.h"
#include "usb_msd.h"

#define BLOCKDEV_MAX        16
#define BLOCKDEV_NAME_LEN   16
#define BLOCKDEV_FSTYPE_LEN 16
#define BLOCKDEV_MOUNT_LEN  64

typedef enum {
    BLOCKDEV_NONE = 0,
    BLOCKDEV_ATA,
    BLOCKDEV_AHCI,
    BLOCKDEV_NVME,
    BLOCKDEV_USB_MSD,
} blockdev_type_t;

typedef struct blockdev {
    int used;
    blockdev_type_t type;
    char name[BLOCKDEV_NAME_LEN];
    uint32_t sector_size;
    uint64_t total_sectors;

    void *driver_data;
    int port;

    int mounted;
    char mount_point[BLOCKDEV_MOUNT_LEN];
    char fs_type[BLOCKDEV_FSTYPE_LEN];
    void *fs_ctx;
} blockdev_t;

void blockdev_init(void);

int blockdev_register_ata(ata_device_t *dev);
int blockdev_register_ahci(ahci_hba_t *hba, int port, uint64_t sectors);
int blockdev_register_nvme(nvme_device_t *dev);
int blockdev_register_usb(usb_msd_device_t *dev);
int blockdev_unregister(blockdev_t *bd);      /* the device went away */

int blockdev_count(void);
blockdev_t *blockdev_get(int index);
blockdev_t *blockdev_find(const char *name);

int blockdev_flush(blockdev_t *bd);           /* push the device's write cache to media */
void blockdev_cache_invalidate(blockdev_t *bd); /* NULL = every device */
int blockdev_read(blockdev_t *bd, uint64_t lba, uint32_t count, void *buf);
int blockdev_write(blockdev_t *bd, uint64_t lba, uint32_t count, const void *buf);

int blockdev_read_bytes(blockdev_t *bd, uint64_t byte_offset, uint32_t len, void *buf);
int blockdev_write_bytes(blockdev_t *bd, uint64_t byte_offset, uint32_t len, const void *buf);

#endif
