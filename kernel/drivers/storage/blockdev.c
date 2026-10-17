#include "blockdev.h"
#include "memory.h"
#include "string.h"
#include "scheduler.h"

#define BLOCKDEV_CHUNK 128

static blockdev_t devices[BLOCKDEV_MAX];
static int dev_count = 0;

void blockdev_init(void)
{
    memset(devices, 0, sizeof(devices));
    dev_count = 0;
}

static int last_slot;

static blockdev_t *alloc_slot(void)
{
    /* a slot left by a device that went away (a USB disk pulled out) is used again */
    for (int i = 0; i < dev_count; i++)
        if (!devices[i].used) { memset(&devices[i], 0, sizeof(devices[i])); devices[i].used = 1; last_slot = i; return &devices[i]; }
    if (dev_count >= BLOCKDEV_MAX) return 0;
    last_slot = dev_count;
    blockdev_t *bd = &devices[dev_count++];
    memset(bd, 0, sizeof(*bd));
    bd->used = 1;
    return bd;
}

static void set_name(blockdev_t *bd, const char *prefix, int idx)
{
    int i = 0;
    while (prefix[i] && i < BLOCKDEV_NAME_LEN - 4) { bd->name[i] = prefix[i]; i++; }
    bd->name[i++] = '0' + (idx % 10);
    bd->name[i] = 0;
}

int blockdev_register_ata(ata_device_t *dev)
{
    blockdev_t *bd = alloc_slot();
    if (!bd) return -1;
    bd->type = BLOCKDEV_ATA;
    set_name(bd, "ata", dev_count - 1);
    bd->sector_size = 512;
    bd->total_sectors = dev->lba48 ? dev->sectors_48 : (uint64_t)dev->sectors_28;
    bd->driver_data = dev;
    return 0;
}

int blockdev_register_ahci(ahci_hba_t *hba, int port, uint64_t sectors)
{
    blockdev_t *bd = alloc_slot();
    if (!bd) return -1;
    bd->type = BLOCKDEV_AHCI;
    set_name(bd, "ahci", port);
    bd->sector_size = 512;
    bd->total_sectors = sectors;
    bd->driver_data = hba;
    bd->port = port;
    return 0;
}

int blockdev_register_nvme(nvme_device_t *dev)
{
    blockdev_t *bd = alloc_slot();
    if (!bd) return -1;
    bd->type = BLOCKDEV_NVME;
    set_name(bd, "nvme", dev_count - 1);
    bd->sector_size = dev->sector_size ? dev->sector_size : 512;
    bd->total_sectors = dev->total_sectors;
    bd->driver_data = dev;
    return 0;
}

int blockdev_register_usb(usb_msd_device_t *dev)
{
    blockdev_t *bd = alloc_slot();
    if (!bd) return -1;
    bd->type = BLOCKDEV_USB_MSD;
    set_name(bd, "usb", last_slot);
    bd->sector_size = dev->block_size ? dev->block_size : 512;
    bd->total_sectors = dev->block_count;
    bd->driver_data = dev;
    return 0;
}

int blockdev_unregister(blockdev_t *bd)
{
    if (!bd || !bd->used) return -1;
    blockdev_cache_invalidate(bd);
    bd->used = 0;
    bd->mounted = 0;
    bd->driver_data = 0;
    return 0;
}

int blockdev_count(void) { return dev_count; }

blockdev_t *blockdev_get(int index)
{
    if (index < 0 || index >= dev_count || !devices[index].used) return 0;
    return &devices[index];
}

blockdev_t *blockdev_find(const char *name)
{
    for (int i = 0; i < dev_count; i++) {
        if (devices[i].used && strcmp(devices[i].name, name) == 0) return &devices[i];
    }
    return 0;
}

/* Raw device access is not reentrant (PIO command sequences, shared DMA
 * buffers), and the GUI runs several tasks that can reach a disk at once.
 * The scheduler is not running yet during early boot, hence the guard. */
static void bd_lock(void)   { if (task_current()) task_preempt_disable(); }
static void bd_unlock(void) { if (task_current()) task_preempt_enable(); }

/* Write-through sector cache for 512-byte-sector devices. The filesystem
 * drivers read single FAT entries / bitmap bytes through whole-sector PIO
 * reads, which made multi-hundred-KB writes take minutes. Writes always go
 * to the device first, so the cache never holds data the disk lacks. */
#define BDC_ENTRIES 4096

typedef struct { blockdev_t *bd; uint64_t lba; } bdc_tag_t;
static bdc_tag_t *bdc_tags;
static uint8_t *bdc_data;
static int bdc_tried;

static int bdc_ready(void)
{
    if (!bdc_tried) {
        bdc_tried = 1;
        bdc_tags = (bdc_tag_t *)malloc(BDC_ENTRIES * sizeof(bdc_tag_t));
        bdc_data = (uint8_t *)malloc((size_t)BDC_ENTRIES * 512);
        if (!bdc_tags || !bdc_data) {
            if (bdc_tags) free(bdc_tags);
            if (bdc_data) free(bdc_data);
            bdc_tags = 0; bdc_data = 0;
        } else {
            memset(bdc_tags, 0, BDC_ENTRIES * sizeof(bdc_tag_t));
        }
    }
    return bdc_data != 0;
}

static uint32_t bdc_slot(blockdev_t *bd, uint64_t lba)
{
    return (uint32_t)((lba + (uint64_t)(bd - devices) * 977) % BDC_ENTRIES);
}

static uint8_t *bdc_lookup(blockdev_t *bd, uint64_t lba)
{
    uint32_t i = bdc_slot(bd, lba);
    if (bdc_tags[i].bd == bd && bdc_tags[i].lba == lba) return bdc_data + (size_t)i * 512;
    return 0;
}

static void bdc_store(blockdev_t *bd, uint64_t lba, const uint8_t *data)
{
    uint32_t i = bdc_slot(bd, lba);
    bdc_tags[i].bd = bd;
    bdc_tags[i].lba = lba;
    memcpy(bdc_data + (size_t)i * 512, data, 512);
}

static void bdc_drop(blockdev_t *bd, uint64_t lba, uint32_t count)
{
    if (!bdc_data) return;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t i = bdc_slot(bd, lba + k);
        if (bdc_tags[i].bd == bd && bdc_tags[i].lba == lba + k) bdc_tags[i].bd = 0;
    }
}

int blockdev_flush(blockdev_t *bd)
{
    if (!bd || !bd->used) return -1;
    if (bd->type != BLOCKDEV_ATA) return 0;
    bd_lock();
    int rc = ata_flush((ata_device_t *)bd->driver_data);
    bd_unlock();
    return rc;
}

void blockdev_cache_invalidate(blockdev_t *bd)
{
    bd_lock();
    if (bdc_data) {
        for (uint32_t i = 0; i < BDC_ENTRIES; i++)
            if (!bd || bdc_tags[i].bd == bd) bdc_tags[i].bd = 0;
    }
    bd_unlock();
}

static int bdc_usable(blockdev_t *bd) { return bd->sector_size == 512 && bdc_ready(); }

static int raw_read(blockdev_t *bd, uint64_t lba, uint32_t count, void *buf)
{
    switch (bd->type) {
    case BLOCKDEV_ATA:
        return ata_read_sectors((ata_device_t *)bd->driver_data, lba, count, buf);
    case BLOCKDEV_AHCI:
        return ahci_read((ahci_hba_t *)bd->driver_data, bd->port, lba, (int)count, buf);
    case BLOCKDEV_NVME:
        return nvme_read((nvme_device_t *)bd->driver_data, lba, count, buf);
    case BLOCKDEV_USB_MSD:
        return usb_msd_read((usb_msd_device_t *)bd->driver_data, (uint32_t)lba, (uint8_t)count, buf);
    default:
        return -1;
    }
}

static int raw_write(blockdev_t *bd, uint64_t lba, uint32_t count, const void *buf)
{
    switch (bd->type) {
    case BLOCKDEV_ATA:
        return ata_write_sectors((ata_device_t *)bd->driver_data, lba, count, buf);
    case BLOCKDEV_AHCI:
        return ahci_write((ahci_hba_t *)bd->driver_data, bd->port, lba, (int)count, buf);
    case BLOCKDEV_NVME:
        return nvme_write((nvme_device_t *)bd->driver_data, lba, count, buf);
    case BLOCKDEV_USB_MSD:
        return usb_msd_write((usb_msd_device_t *)bd->driver_data, (uint32_t)lba, (uint8_t)count, buf);
    default:
        return -1;
    }
}

int blockdev_read(blockdev_t *bd, uint64_t lba, uint32_t count, void *buf)
{
    if (!bd || !bd->used) return -1;
    if (count == 0 || lba + count > bd->total_sectors) return -1;
    uint8_t *p = (uint8_t *)buf;
    int rc = 0;
    bd_lock();
    if (!bdc_usable(bd)) {
        while (count > 0) {
            uint32_t chunk = count > BLOCKDEV_CHUNK ? BLOCKDEV_CHUNK : count;
            if (raw_read(bd, lba, chunk, p) != 0) { rc = -1; break; }
            lba += chunk;
            p += (uint32_t)chunk * bd->sector_size;
            count -= chunk;
        }
    } else {
        uint32_t i = 0;
        while (i < count) {
            uint8_t *c = bdc_lookup(bd, lba + i);
            if (c) { memcpy(p + (size_t)i * 512, c, 512); i++; continue; }
            uint32_t j = i + 1;
            while (j < count && (j - i) < BLOCKDEV_CHUNK && !bdc_lookup(bd, lba + j)) j++;
            if (raw_read(bd, lba + i, j - i, p + (size_t)i * 512) != 0) { rc = -1; break; }
            for (uint32_t k = i; k < j; k++) bdc_store(bd, lba + k, p + (size_t)k * 512);
            i = j;
        }
    }
    bd_unlock();
    return rc;
}

int blockdev_write(blockdev_t *bd, uint64_t lba, uint32_t count, const void *buf)
{
    if (!bd || !bd->used) return -1;
    if (count == 0 || lba + count > bd->total_sectors) return -1;
    const uint8_t *p = (const uint8_t *)buf;
    int rc = 0;
    int cache = 0;
    bd_lock();
    cache = bdc_usable(bd);
    while (count > 0) {
        uint32_t chunk = count > BLOCKDEV_CHUNK ? BLOCKDEV_CHUNK : count;
        if (raw_write(bd, lba, chunk, p) != 0) {
            if (cache) bdc_drop(bd, lba, chunk);
            rc = -1;
            break;
        }
        if (cache)
            for (uint32_t k = 0; k < chunk; k++) bdc_store(bd, lba + k, p + (size_t)k * 512);
        lba += chunk;
        p += (uint32_t)chunk * bd->sector_size;
        count -= chunk;
    }
    bd_unlock();
    return rc;
}

int blockdev_read_bytes(blockdev_t *bd, uint64_t byte_offset, uint32_t len, void *buf)
{
    if (!bd || !bd->used || len == 0) return -1;
    uint32_t ss = bd->sector_size;
    uint64_t first_lba = byte_offset / ss;
    uint32_t skip = (uint32_t)(byte_offset % ss);
    uint32_t total_bytes = skip + len;
    uint32_t sectors = (total_bytes + ss - 1) / ss;

    uint8_t *tmp = (uint8_t *)malloc(sectors * ss);
    if (!tmp) return -1;
    if (blockdev_read(bd, first_lba, sectors, tmp) != 0) { free(tmp); return -1; }
    memcpy(buf, tmp + skip, len);
    free(tmp);
    return 0;
}

int blockdev_write_bytes(blockdev_t *bd, uint64_t byte_offset, uint32_t len, const void *buf)
{
    if (!bd || !bd->used || len == 0) return -1;
    uint32_t ss = bd->sector_size;
    uint64_t first_lba = byte_offset / ss;
    uint32_t skip = (uint32_t)(byte_offset % ss);
    uint32_t total_bytes = skip + len;
    uint32_t sectors = (total_bytes + ss - 1) / ss;

    uint8_t *tmp = (uint8_t *)malloc(sectors * ss);
    if (!tmp) return -1;

    if (skip != 0 || total_bytes % ss != 0) {
        if (blockdev_read(bd, first_lba, sectors, tmp) != 0) { free(tmp); return -1; }
    }
    memcpy(tmp + skip, buf, len);
    if (blockdev_write(bd, first_lba, sectors, tmp) != 0) { free(tmp); return -1; }
    free(tmp);
    return 0;
}
