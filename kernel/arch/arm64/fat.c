#include "fat.h"
#include "emmc.h"
#include "uart.h"
#include "string_arm.h"

/* Everything needed to find a file in the root directory and follow its cluster chain. */

static uint32_t part_lba;
static uint32_t fat_begin, cluster_begin, sectors_per_cluster, root_cluster;
static int mounted;

static uint8_t sec[512] __attribute__((aligned(8)));

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int fat_mount(void)
{
    mounted = 0;
    if (emmc_read(0, 1, sec) < 0) return -1;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return -1;

    part_lba = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sec + 446 + i * 16;
        uint8_t type = e[4];
        /* 0x0B and 0x0C are FAT32; 0x0E is the LBA-addressed FAT16 some cards still use */
        if (type == 0x0B || type == 0x0C || type == 0x0E) { part_lba = rd32(e + 8); break; }
    }
    if (!part_lba) return -1;

    if (emmc_read(part_lba, 1, sec) < 0) return -1;
    uint32_t bytes_per_sector = rd16(sec + 11);
    sectors_per_cluster = sec[13];
    uint32_t reserved = rd16(sec + 14);
    uint32_t num_fats = sec[16];
    uint32_t fat_size = rd32(sec + 36);
    root_cluster = rd32(sec + 44);
    if (bytes_per_sector != 512 || sectors_per_cluster == 0 || fat_size == 0) return -1;

    fat_begin = part_lba + reserved;
    cluster_begin = fat_begin + num_fats * fat_size;
    mounted = 1;
    return 0;
}

static uint32_t cluster_lba(uint32_t cluster)
{
    return cluster_begin + (cluster - 2) * sectors_per_cluster;
}

/* The next cluster of a chain, or >= 0x0FFFFFF8 at the end. */
static uint32_t next_cluster(uint32_t cluster)
{
    uint32_t off = cluster * 4;
    if (emmc_read(fat_begin + off / 512, 1, sec) < 0) return 0x0FFFFFFF;
    return rd32(sec + (off % 512)) & 0x0FFFFFFF;
}

/* Turns a directory entry's padded 8.3 name into "NAME.EXT". */
static void name_from_entry(const uint8_t *e, char *out)
{
    int o = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[o++] = (char)e[i];
    if (e[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) out[o++] = (char)e[i];
    }
    out[o] = 0;
}

static int name_matches(const char *a, const char *b)
{
    /* FAT stores upper case; a caller should not have to. */
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* Walks the root directory, calling `cb` for each real file; stops early if `want` matches
 * and reports where that file starts. */
static int walk_root(const char *want, uint32_t *out_cluster, uint32_t *out_size,
                     void (*cb)(const char *, uint32_t))
{
    if (!mounted) return -1;
    uint32_t cluster = root_cluster;
    char name[16];
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (emmc_read(cluster_lba(cluster) + s, 1, sec) < 0) return -1;
            for (int i = 0; i < 512; i += 32) {
                const uint8_t *e = sec + i;
                if (e[0] == 0x00) return cb ? 0 : -1;     /* nothing further in this directory */
                if (e[0] == 0xE5) continue;               /* deleted */
                if ((e[11] & 0x0F) == 0x0F) continue;     /* a long-name fragment */
                if (e[11] & 0x08) continue;               /* the volume label */
                if (e[11] & 0x10) continue;               /* a subdirectory */
                name_from_entry(e, name);
                uint32_t size = rd32(e + 28);
                if (cb) cb(name, size);
                if (want && name_matches(name, want)) {
                    *out_cluster = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
                    *out_size = size;
                    return 0;
                }
            }
        }
        cluster = next_cluster(cluster);
    }
    return cb ? 0 : -1;
}

int fat_list(void (*cb)(const char *name, uint32_t size)) { return walk_root(0, 0, 0, cb); }

int fat_read_file(const char *name, uint8_t *buf, uint32_t max)
{
    uint32_t cluster = 0, size = 0;
    if (walk_root(name, &cluster, &size, 0) != 0) return -1;
    if (size > max) return -1;

    uint32_t got = 0;
    while (cluster >= 2 && cluster < 0x0FFFFFF8 && got < size) {
        for (uint32_t s = 0; s < sectors_per_cluster && got < size; s++) {
            if (emmc_read(cluster_lba(cluster) + s, 1, sec) < 0) return -1;
            uint32_t n = size - got;
            if (n > 512) n = 512;
            memcpy(buf + got, sec, n);
            got += n;
        }
        cluster = next_cluster(cluster);
    }
    return (int)got;
}
