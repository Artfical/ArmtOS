#include "vfs.h"
#include "string.h"
#include "memory.h"
#include "terminal.h"
#include "serial.h"
#include "auth.h"
#include "perm.h"

typedef struct {
    int used;
    char path[VFS_NAME_LEN];
    int path_len;
    vfs_ops_t *ops;
    void *private_data;
    int is_bind;                       /* another name for the tree at bind_target */
    char bind_target[VFS_NAME_LEN];
    int synth;                         /* the file system keeps no owners: the mounting user owns everything */
    uint32_t s_uid, s_gid;
} mount_t;

typedef struct {
    int used;
    int fd;
    int mount_idx;
    char path[VFS_NAME_LEN];
    uint32_t offset;
    int flags;
} fd_entry_t;

static mount_t mounts[VFS_MAX_MOUNTS];
static fd_entry_t fd_table[VFS_MAX_FDS];
static int mount_count = 0;
static int next_fd = 3;

static char cwd[VFS_NAME_LEN] = "/";
static int denied_flag;                /* the last refusal was a permission refusal */

int vfs_denied(void) { return denied_flag; }

static void abspath_into(const char *path, char *out);

int vfs_init(void)
{
    memset(mounts, 0, sizeof(mounts));
    memset(fd_table, 0, sizeof(fd_table));
    mount_count = 0;
    next_fd = 3;
    serial_write("vfs: initialized\n");
    return 0;
}

int vfs_mount(const char *path, vfs_ops_t *ops, void *private_data)
{
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used) { slot = i; break; }
    }
    if (slot < 0) return -1;

    mount_t *m = &mounts[slot];
    memset(m, 0, sizeof(*m));
    int i = 0;
    while (path[i] && i < VFS_NAME_LEN - 1) { m->path[i] = path[i]; i++; }
    m->path[i] = 0;
    m->path_len = i;
    m->ops = ops;
    m->private_data = private_data;
    if (m->path_len > 1 && ops && !ops->setattr) {
        m->synth = 1;
        auth_session(&m->s_uid, &m->s_gid);
    }
    m->used = 1;
    if (slot >= mount_count) mount_count = slot + 1;
    return 0;
}

int vfs_bind(const char *path, const char *target)
{
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used) { slot = i; break; }
    }
    if (slot < 0 || path[0] != '/' || target[0] != '/') return -1;
    mount_t *m = &mounts[slot];
    memset(m, 0, sizeof(*m));
    strncpy(m->path, path, VFS_NAME_LEN - 1);
    m->path_len = (int)strlen(m->path);
    strncpy(m->bind_target, target, VFS_NAME_LEN - 1);
    m->is_bind = 1;
    m->used = 1;
    if (slot >= mount_count) mount_count = slot + 1;
    return 0;
}

int vfs_unmount(const char *path)
{
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].used) continue;
        if (mounts[i].path_len == 0) continue;
        if (strcmp(mounts[i].path, abs) == 0) {
            for (int f = 0; f < VFS_MAX_FDS; f++) {
                if (fd_table[f].used && fd_table[f].mount_idx == i) return -1;
            }
            mounts[i].used = 0;
            mounts[i].ops = 0;
            mounts[i].private_data = 0;
            return 0;
        }
    }
    return -1;
}

/* the device under the mount is gone: drop it even with files open (they read as closed) */
int vfs_unmount_force(const char *path)
{
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].used || mounts[i].path_len == 0 || strcmp(mounts[i].path, abs)) continue;
        for (int f = 0; f < VFS_MAX_FDS; f++)
            if (fd_table[f].used && fd_table[f].mount_idx == i) fd_table[f].used = 0;
        mounts[i].used = 0;
        mounts[i].ops = 0;
        mounts[i].private_data = 0;
        return 0;
    }
    return -1;
}

static mount_t *find_mount(const char *path)
{
    mount_t *best = 0;
    int best_len = -1;
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].used) continue;
        int match = 1;
        for (int j = 0; j < mounts[i].path_len; j++) {
            if (mounts[i].path[j] != path[j]) { match = 0; break; }
        }
        if (match && mounts[i].path_len > best_len) {
            int plen = mounts[i].path_len;
            int ends_with_slash = plen > 0 && mounts[i].path[plen - 1] == '/';
            if (ends_with_slash || path[plen] == '/' || path[plen] == 0) {
                best = &mounts[i];
                best_len = plen;
            }
        }
    }
    return best;
}

/* The mount that serves `abs`, following bind mounts: abs is rewritten to the
 * path inside the real mount. */
static mount_t *resolve_mount(char *abs)
{
    for (int depth = 0; depth < 4; depth++) {
        mount_t *m = find_mount(abs);
        if (!m) return 0;
        if (!m->is_bind) return m;
        char tmp[VFS_NAME_LEN];
        const char *rest = abs + m->path_len;
        int k = 0;
        for (const char *t = m->bind_target; *t && k < VFS_NAME_LEN - 1; t++) tmp[k++] = *t;
        if (*rest && k > 0 && tmp[k - 1] == '/' && *rest == '/') rest++;
        while (*rest && k < VFS_NAME_LEN - 1) tmp[k++] = *rest++;
        tmp[k] = 0;
        strcpy(abs, tmp);
    }
    return 0;
}

static const char *strip_mount(const char *path, mount_t *m)
{
    if (m->path_len == 0) return path;
    const char *sub = path + m->path_len;
    if (*sub == '/') sub++;
    return sub;
}

static void abspath_into(const char *path, char *out)
{
    const char *r = vfs_abspath(path);
    int i = 0;
    while (r[i] && i < VFS_NAME_LEN - 1) { out[i] = r[i]; i++; }
    out[i] = 0;
}

/* ---------- permissions ---------- */

static int has_perms(mount_t *m)
{
    return m && m->ops && (m->ops->setattr != 0 || m->synth);
}

/* owner and mode of an entry on a file system without ownership (like mounting vfat with uid=, umask=022) */
static void synth_owner(mount_t *m, vfs_entry_t *e)
{
    if (!m->synth) return;
    e->uid = m->s_uid;
    e->gid = m->s_gid;
    e->mode = e->is_dir ? 0755u : 0644u;
}

static int stat_abs(mount_t *m, const char *abs, vfs_entry_t *e)
{
    if (!m->ops || !m->ops->stat) return -1;
    memset(e, 0, sizeof(*e));
    int rc = m->ops->stat(m->private_data, strip_mount(abs, m), e);
    if (rc == 0) synth_owner(m, e);
    return rc;
}

static void parent_of(const char *abs, char *out)
{
    int n = (int)strlen(abs);
    while (n > 1 && abs[n - 1] == '/') n--;
    while (n > 1 && abs[n - 1] != '/') n--;
    if (n > 1) n--;
    memcpy(out, abs, (size_t)n);
    out[n] = 0;
    if (n == 0) { out[0] = '/'; out[1] = 0; }
}

/* x on every directory leading to abs (not on abs itself) */
static int walk_ok(mount_t *m, const char *abs)
{
    char prefix[VFS_NAME_LEN];
    int n = (int)strlen(abs);
    for (int i = 1; i < n; i++) {
        if (abs[i] != '/') continue;
        memcpy(prefix, abs, (size_t)i);
        prefix[i] = 0;
        if (m->path_len > 1 && i <= m->path_len) continue;
        vfs_entry_t e;
        if (stat_abs(m, prefix, &e) != 0) return 1;           /* missing: the real operation reports it */
        if (e.is_dir && !perm_mode_allows(e.mode, e.uid, e.gid, VFS_ACC_X)) { denied_flag = 1; return 0; }
    }
    return 1;
}

/* may the current user do `want` to the existing object at abs? */
static int may_access(mount_t *m, const char *abs, int want)
{
    if (!has_perms(m) || auth_is_root()) return 1;
    if (!walk_ok(m, abs)) return 0;
    vfs_entry_t e;
    if (stat_abs(m, abs, &e) != 0) return 1;
    if (!perm_mode_allows(e.mode, e.uid, e.gid, want)) { denied_flag = 1; return 0; }
    return 1;
}

/* may entries be added to / removed from the directory holding abs? */
static int may_change_dir(mount_t *m, const char *abs)
{
    if (!has_perms(m) || auth_is_root()) return 1;
    if (!walk_ok(m, abs)) return 0;
    char parent[VFS_NAME_LEN];
    parent_of(abs, parent);
    vfs_entry_t e;
    if (stat_abs(m, parent, &e) != 0) return 1;
    if (!perm_mode_allows(e.mode, e.uid, e.gid, VFS_ACC_W | VFS_ACC_X)) { denied_flag = 1; return 0; }
    return 1;
}

/* a sticky directory (like /tmp) lets only the owner of an entry or of the directory remove it */
static int sticky_ok(mount_t *m, const char *abs)
{
    if (!has_perms(m) || auth_is_root()) return 1;
    char parent[VFS_NAME_LEN];
    parent_of(abs, parent);
    vfs_entry_t pe, fe;
    if (stat_abs(m, parent, &pe) != 0 || !(pe.mode & PERM_STICKY)) return 1;
    if (stat_abs(m, abs, &fe) != 0) return 1;
    uint32_t uid = auth_uid();
    if (uid == fe.uid || uid == pe.uid) return 1;
    denied_flag = 1;
    return 0;
}

/* a new file or directory belongs to whoever made it */
static void own_new(mount_t *m, const char *abs, int is_dir)
{
    if (!has_perms(m) || !m->ops->setattr) return;
    uint32_t base = is_dir ? 0777u : 0666u;
    m->ops->setattr(m->private_data, strip_mount(abs, m), base & ~perm_umask(), auth_uid(), auth_gid());
}

/* ---------- operations ---------- */

int vfs_open(const char *path, int flags)
{
    denied_flag = 0;
    char orig[VFS_NAME_LEN], abs[VFS_NAME_LEN];
    abspath_into(path, orig);
    strcpy(abs, orig);

    mount_t *m = resolve_mount(abs);
    if (!m || !m->ops || !m->ops->open) return -1;

    const char *sub = strip_mount(abs, m);
    int wants_write = (flags & (VFS_WRONLY | VFS_RDWR | VFS_TRUNC | VFS_APPEND)) != 0;
    int created = 0;
    if (has_perms(m)) {
        vfs_entry_t e;
        int exists = stat_abs(m, abs, &e) == 0;
        if (exists) {
            if (!may_access(m, abs, wants_write ? VFS_ACC_W : VFS_ACC_R)) return -1;
            if (wants_write && perm_is_system_path(orig) && !e.is_dir && !auth_is_root()) { denied_flag = 1; return -1; }
        } else if (flags & (VFS_CREAT | 2)) {
            if (!may_change_dir(m, abs)) return -1;
            created = 1;
        }
    }
    int fd = m->ops->open(m->private_data, sub, flags);
    if (fd < 0) return -1;
    if (created) own_new(m, abs, 0);

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fd_table[i].used) {
            fd_table[i].used = 1;
            fd_table[i].fd = fd;
            fd_table[i].mount_idx = (int)(m - mounts);
            int j = 0;
            while (abs[j] && j < VFS_NAME_LEN - 1) { fd_table[i].path[j] = abs[j]; j++; }
            fd_table[i].path[j] = 0;
            fd_table[i].offset = 0;
            fd_table[i].flags = flags;
            return i;
        }
    }
    if (m->ops->close) m->ops->close(m->private_data, fd);
    return -1;
}

int vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -1;
    mount_t *m = &mounts[fd_table[fd].mount_idx];
    int ret = 0;
    if (m->ops && m->ops->close)
        ret = m->ops->close(m->private_data, fd_table[fd].fd);
    fd_table[fd].used = 0;
    return ret;
}

int vfs_read(int fd, void *buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -1;
    mount_t *m = &mounts[fd_table[fd].mount_idx];
    if (!m->ops || !m->ops->read) return -1;
    return m->ops->read(m->private_data, fd_table[fd].fd, buf, size);
}

int vfs_write(int fd, const void *buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -1;
    mount_t *m = &mounts[fd_table[fd].mount_idx];
    if (!m->ops || !m->ops->write) return -1;
    return m->ops->write(m->private_data, fd_table[fd].fd, buf, size);
}

int vfs_lseek(int fd, uint32_t offset, int whence)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fd_table[fd].used) return -1;
    mount_t *m = &mounts[fd_table[fd].mount_idx];
    if (!m->ops || !m->ops->lseek) return -1;
    return m->ops->lseek(m->private_data, fd_table[fd].fd, offset, whence);
}

int vfs_readdir(const char *path, vfs_entry_t *entries, int max)
{
    denied_flag = 0;
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = resolve_mount(abs);
    if (!m || !m->ops || !m->ops->readdir) return -1;
    if (!may_access(m, abs, VFS_ACC_R)) return -1;
    int n = m->ops->readdir(m->private_data, strip_mount(abs, m), entries, max);
    if (n > 0 && m->synth) for (int i = 0; i < n; i++) synth_owner(m, &entries[i]);
    return n;
}

int vfs_mkdir(const char *path, uint32_t mode)
{
    denied_flag = 0;
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = resolve_mount(abs);
    if (!m || !m->ops || !m->ops->mkdir) return -1;
    if (!may_change_dir(m, abs)) return -1;
    int rc = m->ops->mkdir(m->private_data, strip_mount(abs, m), mode);
    if (rc == 0) own_new(m, abs, 1);
    return rc;
}

/* may the current user remove this entry (directory write access, sticky bit, the system's password)? */
int vfs_check_remove(const char *path)
{
    denied_flag = 0;
    char orig[VFS_NAME_LEN], abs[VFS_NAME_LEN];
    abspath_into(path, orig);
    strcpy(abs, orig);
    mount_t *m = resolve_mount(abs);
    if (!m) return -1;
    if (!may_change_dir(m, abs) || !sticky_ok(m, abs)) return -1;
    if (perm_guard_system(orig, "delete") != 0) { denied_flag = 1; return -1; }
    return 0;
}

int vfs_unlink(const char *path)
{
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = resolve_mount(abs);
    if (!m || !m->ops || !m->ops->unlink) return -1;
    if (vfs_check_remove(path) != 0) return -1;
    return m->ops->unlink(m->private_data, strip_mount(abs, m));
}

int vfs_stat(const char *path, vfs_entry_t *entry)
{
    denied_flag = 0;
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = resolve_mount(abs);
    if (!m || !m->ops || !m->ops->stat) return -1;
    if (has_perms(m) && !auth_is_root() && !walk_ok(m, abs)) return -1;
    int rc = m->ops->stat(m->private_data, strip_mount(abs, m), entry);
    if (rc == 0) synth_owner(m, entry);
    return rc;
}

int vfs_rename(const char *old, const char *new_path)
{
    denied_flag = 0;
    char orig_old[VFS_NAME_LEN], abs_old[VFS_NAME_LEN], abs_new[VFS_NAME_LEN];
    abspath_into(old, orig_old);
    strcpy(abs_old, orig_old);
    abspath_into(new_path, abs_new);
    char orig_new[VFS_NAME_LEN];
    strcpy(orig_new, abs_new);
    mount_t *m = resolve_mount(abs_old);
    if (!m || !m->ops || !m->ops->rename) return -1;
    mount_t *m2 = resolve_mount(abs_new);
    if (m != m2) return -1;
    if (!may_change_dir(m, abs_old) || !sticky_ok(m, abs_old) || !may_change_dir(m, abs_new)) return -1;
    if (perm_guard_system(orig_old, "move") != 0) return -1;
    if (vfs_exists(orig_new) && perm_guard_system(orig_new, "replace") != 0) return -1;
    return m->ops->rename(m->private_data, strip_mount(abs_old, m), strip_mount(abs_new, m2));
}

int vfs_symlink(const char *target, const char *path)
{
    denied_flag = 0;
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = resolve_mount(abs);
    if (!m || !m->ops || !m->ops->symlink) return -1;
    if (!may_change_dir(m, abs)) return -1;
    int rc = m->ops->symlink(m->private_data, target, strip_mount(abs, m));
    if (rc == 0) own_new(m, abs, 0);
    return rc;
}

int vfs_chmod(const char *path, uint32_t mode)
{
    denied_flag = 0;
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = resolve_mount(abs);
    if (!m || !m->ops || !m->ops->setattr) return -1;
    vfs_entry_t e;
    if (stat_abs(m, abs, &e) != 0) return -1;
    if (!auth_is_root() && auth_uid() != e.uid) { denied_flag = 1; return -1; }
    return m->ops->setattr(m->private_data, strip_mount(abs, m), mode & 07777, VFS_KEEP, VFS_KEEP);
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    denied_flag = 0;
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = resolve_mount(abs);
    if (!m || !m->ops || !m->ops->setattr) return -1;
    vfs_entry_t e;
    if (stat_abs(m, abs, &e) != 0) return -1;
    if (!auth_is_root()) {
        /* only the owner, and only to a group he belongs to; giving files away is for root */
        if (auth_uid() != e.uid || (uid != VFS_KEEP && uid != e.uid) ||
            (gid != VFS_KEEP && gid != auth_gid() && !auth_in_gid(auth_uid(), gid))) { denied_flag = 1; return -1; }
    }
    return m->ops->setattr(m->private_data, strip_mount(abs, m), VFS_KEEP, uid, gid);
}

int vfs_chmod_supported(const char *path)
{
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = resolve_mount(abs);
    return m && m->ops && m->ops->setattr;
}

int vfs_access(const char *path, int want)
{
    char abs[VFS_NAME_LEN];
    abspath_into(path, abs);
    mount_t *m = resolve_mount(abs);
    if (!m) return -1;
    vfs_entry_t e;
    if (stat_abs(m, abs, &e) != 0) return -1;
    if (!has_perms(m)) return 0;
    if (!walk_ok(m, abs)) return -1;
    return perm_mode_allows(e.mode, e.uid, e.gid, want) ? 0 : -1;
}

int vfs_exists(const char *path)
{
    vfs_entry_t e;
    return vfs_stat(path, &e) == 0;
}

int vfs_path_has_mount(const char *path)
{
    if (!path || path[0] != '/') return 0;
    mount_t *m = find_mount(path);
    return (m != 0 && m->path_len > 0);
}

int vfs_get_mounts(char out[][VFS_NAME_LEN], int max)
{
    int n = 0;
    for (int i = 0; i < mount_count && n < max; i++) {
        if (!mounts[i].used || mounts[i].is_bind) continue;
        int j = 0;
        while (mounts[i].path[j] && j < VFS_NAME_LEN - 1) { out[n][j] = mounts[i].path[j]; j++; }
        out[n][j] = 0;
        n++;
    }
    return n;
}

void vfs_chdir(const char *path)
{
    abspath_into(path, cwd);
}

char *vfs_abspath(const char *path)
{
    static char buf[VFS_NAME_LEN];
    if (path[0] == '/') {
        int i = 0;
        while (path[i] && i < VFS_NAME_LEN - 1) { buf[i] = path[i]; i++; }
        buf[i] = 0;
    } else {
        int i = 0;
        while (cwd[i] && i < VFS_NAME_LEN - 1) { buf[i] = cwd[i]; i++; }
        if (buf[i-1] != '/') { buf[i] = '/'; i++; }
        int j = 0;
        while (path[j] && i < VFS_NAME_LEN - 1) { buf[i] = path[j]; i++; j++; }
        buf[i] = 0;
    }
    return buf;
}
