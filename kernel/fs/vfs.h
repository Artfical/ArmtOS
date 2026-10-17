#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define VFS_MAX_FDS      128
#define VFS_MAX_MOUNTS   16
#define VFS_NAME_LEN     128

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

#define VFS_RDONLY  0
#define VFS_WRONLY  1
#define VFS_RDWR    2
#define VFS_CREAT   0x100
#define VFS_TRUNC   0x200
#define VFS_APPEND  0x400

typedef struct vfs_entry {
    char name[VFS_NAME_LEN];
    uint32_t size;
    int is_dir;
    uint32_t inode;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
} vfs_entry_t;

typedef struct vfs_ops {
    int   (*open)(void *ctx, const char *path, int flags);
    int   (*close)(void *ctx, int fd);
    int   (*read)(void *ctx, int fd, void *buf, uint32_t size);
    int   (*write)(void *ctx, int fd, const void *buf, uint32_t size);
    int   (*lseek)(void *ctx, int fd, uint32_t offset, int whence);
    int   (*readdir)(void *ctx, const char *path, vfs_entry_t *entries, int max);
    int   (*mkdir)(void *ctx, const char *path, uint32_t mode);
    int   (*unlink)(void *ctx, const char *path);
    int   (*stat)(void *ctx, const char *path, vfs_entry_t *entry);
    int   (*rename)(void *ctx, const char *oldpath, const char *newpath);
    int   (*symlink)(void *ctx, const char *target, const char *path);
    /* change permission bits (mode != VFS_KEEP) and/or owner (uid/gid != VFS_KEEP); a file system that
     * stores no ownership leaves this NULL and is then not subject to permission checks */
    int   (*setattr)(void *ctx, const char *path, uint32_t mode, uint32_t uid, uint32_t gid);
} vfs_ops_t;

#define VFS_KEEP 0xFFFFFFFFu

/* permission bits asked of vfs_access() */
#define VFS_ACC_R 4
#define VFS_ACC_W 2
#define VFS_ACC_X 1

int  vfs_init(void);
int  vfs_mount(const char *path, vfs_ops_t *ops, void *private_data);
int  vfs_unmount(const char *path);
int  vfs_unmount_force(const char *path);     /* also with files open: the device is gone */
int  vfs_open(const char *path, int flags);
int  vfs_close(int fd);
int  vfs_read(int fd, void *buf, uint32_t size);
int  vfs_write(int fd, const void *buf, uint32_t size);
int  vfs_lseek(int fd, uint32_t offset, int whence);
int  vfs_readdir(const char *path, vfs_entry_t *entries, int max);
int  vfs_mkdir(const char *path, uint32_t mode);
int  vfs_unlink(const char *path);
int  vfs_stat(const char *path, vfs_entry_t *entry);
int  vfs_rename(const char *oldpath, const char *newpath);
int  vfs_symlink(const char *target, const char *path);
int  vfs_denied(void);                       /* was the last refused call a permission refusal? */
int  vfs_chmod_supported(const char *path);  /* does the file system under path keep owners and modes? */
int  vfs_check_remove(const char *path);     /* 0 = the current user may remove this entry */
int  vfs_chmod(const char *path, uint32_t mode);
int  vfs_chown(const char *path, uint32_t uid, uint32_t gid);
/* may the current user do `want` (VFS_ACC_*) to this file/directory? 0 = yes */
int  vfs_access(const char *path, int want);
/* makes `path` another name for the tree at `target` (a bind mount) */
int  vfs_bind(const char *path, const char *target);
int  vfs_exists(const char *path);
char *vfs_abspath(const char *path);
void vfs_chdir(const char *path);
int  vfs_path_has_mount(const char *path);
int  vfs_get_mounts(char out[][VFS_NAME_LEN], int max);

#endif
