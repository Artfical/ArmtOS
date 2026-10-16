#include "fat.h"
#include "emmc.h"
#include "uart.h"
#include "string_arm.h"

/* Everything needed to find a file in the root directory and follow its cluster chain. */

static uint32_t part_lba;
static uint32_t fat_begin, cluster_begin, sectors_per_cluster, root_cluster;
static uint32_t num_fats_g, fat_size_g, fsinfo_sector, total_clusters;
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
    num_fats_g = num_fats;
    fat_size_g = fat_size;
    fsinfo_sector = rd16(sec + 48);
    uint32_t total = rd16(sec + 19) ? rd16(sec + 19) : rd32(sec + 32);
    total_clusters = (total - reserved - num_fats * fat_size) / sectors_per_cluster;
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

/* ---- writing ----------------------------------------------------------------------------
 * Enough to keep small files in the root directory (config.txt above all): overwrite a file
 * in place, grow or shrink its cluster chain, or create it. Long file names and
 * subdirectories are not written. Every change goes to every copy of the FAT. */

static uint8_t wsec[512] __attribute__((aligned(8)));
static uint8_t fsec[512] __attribute__((aligned(8)));

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Sets one FAT entry in every copy of the table. */
static int set_fat(uint32_t cluster, uint32_t value)
{
    uint32_t off = cluster * 4;
    for (uint32_t f = 0; f < num_fats_g; f++) {
        uint32_t lba = fat_begin + f * fat_size_g + off / 512;
        if (emmc_read(lba, 1, fsec) < 0) return -1;
        uint32_t old = rd32(fsec + off % 512);
        wr32(fsec + off % 512, (old & 0xF0000000u) | (value & 0x0FFFFFFFu));
        if (emmc_write(lba, 1, fsec) < 0) return -1;
    }
    return 0;
}

static uint32_t alloc_cluster(void)
{
    uint32_t per_sector = 128;
    for (uint32_t sct = 0; sct < fat_size_g; sct++) {
        if (emmc_read(fat_begin + sct, 1, fsec) < 0) return 0;
        for (uint32_t i = 0; i < per_sector; i++) {
            uint32_t c = sct * per_sector + i;
            if (c < 3 || c >= total_clusters + 2) continue;
            if ((rd32(fsec + i * 4) & 0x0FFFFFFF) == 0) {
                if (set_fat(c, 0x0FFFFFFF) != 0) return 0;
                return c;
            }
        }
    }
    return 0;
}

/* Frees a chain starting at `c`. */
static void free_chain(uint32_t c)
{
    while (c >= 2 && c < 0x0FFFFFF8) {
        uint32_t n = next_cluster(c);
        set_fat(c, 0);
        c = n;
    }
}

/* The free-space figures in the FSInfo sector are only hints; after a change they are marked
 * unknown so no driver trusts a stale count. */
static void fsinfo_invalidate(void)
{
    if (fsinfo_sector == 0 || fsinfo_sector == 0xFFFF) return;
    if (emmc_read(part_lba + fsinfo_sector, 1, fsec) < 0) return;
    if (rd32(fsec) != 0x41615252 || rd32(fsec + 484) != 0x61417272) return;
    wr32(fsec + 488, 0xFFFFFFFF);
    wr32(fsec + 492, 0xFFFFFFFF);
    emmc_write(part_lba + fsinfo_sector, 1, fsec);
}

static int to_83(const char *name, uint8_t *out)
{
    memset(out, ' ', 11);
    int i = 0, o = 0;
    for (; name[i] && name[i] != '.'; i++) {
        if (o >= 8) return -1;
        char c = name[i];
        out[o++] = (uint8_t)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }
    if (name[i] == '.') {
        i++;
        for (o = 8; name[i]; i++) {
            if (o >= 11) return -1;
            char c = name[i];
            out[o++] = (uint8_t)((c >= 'a' && c <= 'z') ? c - 32 : c);
        }
    }
    return 0;
}

int fat_write_file(const char *name, const uint8_t *data, uint32_t len)
{
    if (!mounted) return -1;
    uint8_t n11[11];
    if (to_83(name, n11) != 0) return -1;

    /* find the entry, remembering the first free slot in case it has to be created */
    uint64_t ent_lba = 0, free_lba = 0;
    int ent_off = -1, free_off = -1;
    uint32_t start = 0;
    uint32_t dc = root_cluster;
    int done = 0;
    while (!done && dc >= 2 && dc < 0x0FFFFFF8) {
        for (uint32_t s = 0; s < sectors_per_cluster && !done; s++) {
            uint64_t lba = cluster_lba(dc) + s;
            if (emmc_read(lba, 1, wsec) < 0) return -1;
            for (int i = 0; i < 512; i += 32) {
                const uint8_t *e = wsec + i;
                if (e[0] == 0x00 || e[0] == 0xE5) {
                    if (free_off < 0) { free_lba = lba; free_off = i; }
                    if (e[0] == 0x00) { done = 1; break; }
                    continue;
                }
                if ((e[11] & 0x0F) == 0x0F || (e[11] & 0x18)) continue;
                if (!memcmp(e, n11, 11)) {
                    ent_lba = lba; ent_off = i;
                    start = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26);
                    done = 1;
                    break;
                }
            }
        }
        if (!done) dc = next_cluster(dc);
    }
    int creating = ent_off < 0;
    if (creating && free_off < 0) return -1;            /* the directory is full */

    if (start < 2) {
        start = alloc_cluster();
        if (!start) return -1;
    }

    /* write the data, extending the chain when it runs out */
    uint32_t cur = start, done_bytes = 0, bytes_per_cluster = sectors_per_cluster * 512;
    for (;;) {
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (done_bytes >= len) break;
            uint32_t n = len - done_bytes;
            if (n > 512) n = 512;
            memset(wsec, 0, 512);
            memcpy(wsec, data + done_bytes, n);
            if (emmc_write(cluster_lba(cur) + s, 1, wsec) < 0) return -1;
            done_bytes += n;
        }
        if (done_bytes >= len) break;
        uint32_t nx = next_cluster(cur);
        if (nx < 2 || nx >= 0x0FFFFFF8) {
            nx = alloc_cluster();
            if (!nx || set_fat(cur, nx) != 0) return -1;
        }
        cur = nx;
        (void)bytes_per_cluster;
    }
    /* whatever of the old chain is left over is released */
    uint32_t tail = next_cluster(cur);
    set_fat(cur, 0x0FFFFFFF);
    if (tail >= 2 && tail < 0x0FFFFFF8) free_chain(tail);

    /* the directory entry */
    uint64_t lba = creating ? free_lba : ent_lba;
    int off = creating ? free_off : ent_off;
    if (emmc_read(lba, 1, wsec) < 0) return -1;
    uint8_t *e = wsec + off;
    if (creating) {
        memset(e, 0, 32);
        memcpy(e, n11, 11);
        e[11] = 0x20;                                       /* archive */
        e[16] = e[18] = e[24] = 0x21; e[17] = e[19] = e[25] = 0x5C;   /* 2026-01-01: no clock at this layer */
    }
    e[20] = (uint8_t)(start >> 16); e[21] = (uint8_t)(start >> 24);
    e[26] = (uint8_t)start;         e[27] = (uint8_t)(start >> 8);
    wr32(e + 28, len);
    if (emmc_write(lba, 1, wsec) < 0) return -1;

    fsinfo_invalidate();
    return (int)len;
}
