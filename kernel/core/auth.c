#include "auth.h"
#include "scheduler.h"
#include "vfs.h"
#include "fsbridge.h"
#include "ramfs.h"
#include "string.h"
#include "memory.h"
#include "sha256.h"
#include "csprng.h"
#include "keyboard.h"
#include "terminal.h"
#include "klog.h"

#define AUTH_MAX_USERS  32
#define AUTH_MAX_GROUPS 40
#define AUTH_HASH_MAX   112
#define AUTH_ITERATIONS 2048

#define PASSWD_FILE "/etc/passwd"
#define SHADOW_FILE "/etc/shadow"
#define GROUP_FILE  "/etc/group"
#define HOSTNAME_FILE "/etc/hostname"

typedef struct {
    auth_user_t u;
    char hash[AUTH_HASH_MAX];      /* "" = no password, "!" = locked, "$tos$..." = PBKDF2 */
} acct_t;

typedef struct {
    char name[AUTH_NAME_MAX];
    uint32_t gid;
    char members[160];             /* comma separated */
} group_t;

static acct_t accts[AUTH_MAX_USERS];
static int nacct;
static group_t groups[AUTH_MAX_GROUPS];
static int ngroup;
static uint32_t session_uid, session_gid;
static int confirmed;
static char hostname[40] = "tos";

void sprintf_u32(char *out, uint32_t v);

/* ---------- credentials ---------- */

uint32_t auth_uid(void) { uint32_t u, g; task_get_cred(&u, &g); return u; }
uint32_t auth_gid(void) { uint32_t u, g; task_get_cred(&u, &g); return g; }
int auth_is_root(void) { return auth_uid() == 0; }

void auth_set_session(uint32_t uid, uint32_t gid)
{
    task_set_cred(uid, gid);
    session_uid = uid;
    session_gid = gid;
}

void auth_session(uint32_t *uid, uint32_t *gid) { *uid = session_uid; *gid = session_gid; }

void auth_become(uint32_t uid, uint32_t gid, auth_saved_t *saved)
{
    task_get_cred(&saved->uid, &saved->gid);
    task_set_cred(uid, gid);
}

void auth_restore(const auth_saved_t *saved)
{
    task_set_cred(saved->uid, saved->gid);
}

/* ---------- password hashing: PBKDF2-HMAC-SHA256 ---------- */

static void pbkdf2(const uint8_t *pw, uint32_t pwlen, const uint8_t *salt, uint32_t saltlen, uint32_t iters, uint8_t out[32])
{
    uint8_t msg[32 + 4], u[32], v[32];
    memcpy(msg, salt, saltlen);
    msg[saltlen] = 0; msg[saltlen + 1] = 0; msg[saltlen + 2] = 0; msg[saltlen + 3] = 1;
    hmac_sha256(pw, pwlen, msg, saltlen + 4, u);
    memcpy(out, u, 32);
    for (uint32_t i = 1; i < iters; i++) {
        hmac_sha256(pw, pwlen, u, 32, v);
        memcpy(u, v, 32);
        for (int k = 0; k < 32; k++) out[k] ^= u[k];
    }
}

static void to_hex(const uint8_t *in, int n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2 * i] = hx[in[i] >> 4]; out[2 * i + 1] = hx[in[i] & 15]; }
    out[2 * n] = 0;
}

static int from_hex(const char *in, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = in[2 * i + k];
            int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
            if (d < 0) return -1;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

static void make_hash(const char *password, char *out)
{
    uint8_t salt[16], dk[32];
    csprng_fill(salt, 16);
    pbkdf2((const uint8_t *)password, (uint32_t)strlen(password), salt, 16, AUTH_ITERATIONS, dk);
    char sh[33], dh[65];
    to_hex(salt, 16, sh);
    to_hex(dk, 32, dh);
    strcpy(out, "$tos$2048$");
    strcat(out, sh);
    strcat(out, "$");
    strcat(out, dh);
}

static int verify_hash(const char *hash, const char *password)
{
    if (hash[0] == 0) return password[0] == 0 ? 0 : -1;       /* no password set */
    if (strncmp(hash, "$tos$2048$", 10) != 0) return -1;      /* locked ("!") or unknown format */
    uint8_t salt[16], want[32], dk[32];
    if (strlen(hash) != 10 + 32 + 1 + 64 || from_hex(hash + 10, salt, 16) != 0 || hash[42] != '$' ||
        from_hex(hash + 43, want, 32) != 0) return -1;
    pbkdf2((const uint8_t *)password, (uint32_t)strlen(password), salt, 16, AUTH_ITERATIONS, dk);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(dk[i] ^ want[i]);   /* constant time */
    return diff == 0 ? 0 : -1;
}

/* ---------- account files ---------- */

static int split_fields(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < max; p++)
        if (*p == ':') { *p = 0; f[n++] = p + 1; }
    return n;
}

static uint32_t parse_u32(const char *s)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

static void copy_str(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static char *read_file(const char *path, uint32_t *len)
{
    if (!fsbridge_exists(path) || fsbridge_is_dir(path)) return 0;
    uint32_t sz = fsbridge_size(path);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) return 0;
    int n = sz ? fsbridge_read(path, buf, sz, 0) : 0;
    if (n < 0) n = 0;
    buf[n] = 0;
    *len = (uint32_t)n;
    return buf;
}

static void add_default_groups(void)
{
    if (ngroup == 0) {
        copy_str(groups[0].name, "root", AUTH_NAME_MAX); groups[0].gid = 0; groups[0].members[0] = 0;
        copy_str(groups[1].name, AUTH_SUDO_GROUP, AUTH_NAME_MAX); groups[1].gid = AUTH_SUDO_GID; groups[1].members[0] = 0;
        copy_str(groups[2].name, "users", AUTH_NAME_MAX); groups[2].gid = AUTH_USERS_GID; groups[2].members[0] = 0;
        ngroup = 3;
    }
}

static void load_accounts(void)
{
    nacct = 0;
    ngroup = 0;
    uint32_t len;
    char *buf = read_file(PASSWD_FILE, &len);
    if (buf) {
        for (char *p = buf; *p;) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = 0;
            char *f[8];
            if (split_fields(line, f, 7) < 7 || nacct >= AUTH_MAX_USERS || !f[0][0]) continue;
            acct_t *a = &accts[nacct++];
            memset(a, 0, sizeof(*a));
            copy_str(a->u.name, f[0], AUTH_NAME_MAX);
            a->u.uid = parse_u32(f[2]);
            a->u.gid = parse_u32(f[3]);
            copy_str(a->u.gecos, f[4], sizeof(a->u.gecos));
            copy_str(a->u.home, f[5], sizeof(a->u.home));
            copy_str(a->u.shell, f[6], sizeof(a->u.shell));
            copy_str(a->hash, "!", AUTH_HASH_MAX);
        }
        free(buf);
    }
    buf = read_file(SHADOW_FILE, &len);
    if (buf) {
        for (char *p = buf; *p;) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = 0;
            char *f[4];
            if (split_fields(line, f, 3) < 2) continue;
            for (int i = 0; i < nacct; i++)
                if (strcmp(accts[i].u.name, f[0]) == 0) copy_str(accts[i].hash, f[1], AUTH_HASH_MAX);
        }
        free(buf);
    }
    buf = read_file(GROUP_FILE, &len);
    if (buf) {
        for (char *p = buf; *p;) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = 0;
            char *f[5];
            if (split_fields(line, f, 4) < 3 || ngroup >= AUTH_MAX_GROUPS || !f[0][0]) continue;
            group_t *g = &groups[ngroup++];
            memset(g, 0, sizeof(*g));
            copy_str(g->name, f[0], AUTH_NAME_MAX);
            g->gid = parse_u32(f[2]);
            if (f[3]) copy_str(g->members, f[3], sizeof(g->members));
        }
        free(buf);
    }
    if (nacct == 0) {                                    /* nothing installed yet: a root account without a password */
        acct_t *a = &accts[nacct++];
        memset(a, 0, sizeof(*a));
        copy_str(a->u.name, "root", AUTH_NAME_MAX);
        a->u.uid = 0; a->u.gid = 0;
        copy_str(a->u.gecos, "root", sizeof(a->u.gecos));
        copy_str(a->u.home, "/root", sizeof(a->u.home));
        copy_str(a->u.shell, "/system/shell", sizeof(a->u.shell));
        a->hash[0] = 0;
    }
    add_default_groups();
}

static int put_file(const char *path, const char *data, uint32_t len, uint32_t mode)
{
    /* rewrite from scratch: delete + create keeps no stale tail */
    if (fsbridge_exists(path)) fsbridge_delete(path);
    if (fsbridge_create(path) != 0) return -1;
    if (len && fsbridge_write(path, data, len, 0) != (int)len) return -1;
    vfs_chown(path, 0, 0);
    vfs_chmod(path, mode);
    return 0;
}

int auth_save(void)
{
    auth_saved_t saved;
    auth_become(0, 0, &saved);
    char *pw = (char *)malloc((size_t)nacct * 220 + 64);
    char *sh = (char *)malloc((size_t)nacct * 220 + 64);
    char *gr = (char *)malloc((size_t)ngroup * 260 + 64);
    int rc = -1;
    if (pw && sh && gr) {
        pw[0] = sh[0] = gr[0] = 0;
        for (int i = 0; i < nacct; i++) {
            char num[12];
            const acct_t *a = &accts[i];
            strcat(pw, a->u.name); strcat(pw, ":x:");
            sprintf_u32(num, a->u.uid); strcat(pw, num); strcat(pw, ":");
            sprintf_u32(num, a->u.gid); strcat(pw, num); strcat(pw, ":");
            strcat(pw, a->u.gecos); strcat(pw, ":"); strcat(pw, a->u.home); strcat(pw, ":"); strcat(pw, a->u.shell); strcat(pw, "\n");
            strcat(sh, a->u.name); strcat(sh, ":"); strcat(sh, a->hash); strcat(sh, ":0::::::\n");
        }
        for (int i = 0; i < ngroup; i++) {
            char num[12];
            strcat(gr, groups[i].name); strcat(gr, ":x:");
            sprintf_u32(num, groups[i].gid); strcat(gr, num); strcat(gr, ":"); strcat(gr, groups[i].members); strcat(gr, "\n");
        }
        if (!fsbridge_exists("/etc")) { fsbridge_mkdir("/etc"); vfs_chown("/etc", 0, 0); vfs_chmod("/etc", 0755); }
        rc = 0;
        if (put_file(PASSWD_FILE, pw, (uint32_t)strlen(pw), 0644) != 0) { klog_write("auth: cannot write /etc/passwd\n"); rc = -1; }
        if (put_file(SHADOW_FILE, sh, (uint32_t)strlen(sh), 0600) != 0) rc = -1;
        if (put_file(GROUP_FILE, gr, (uint32_t)strlen(gr), 0644) != 0) rc = -1;
    }
    free(pw); free(sh); free(gr);
    auth_restore(&saved);
    return rc;
}

void auth_init(void)
{
    auth_saved_t saved;
    auth_become(0, 0, &saved);
    load_accounts();
    uint32_t len;
    char *h = read_file(HOSTNAME_FILE, &len);
    if (h) {
        for (uint32_t i = 0; i < len; i++) if (h[i] == '\n' || h[i] == '\r') { h[i] = 0; break; }
        if (h[0]) copy_str(hostname, h, sizeof(hostname));
        free(h);
    }
    auth_restore(&saved);
}

int auth_have_accounts(void)
{
    for (int i = 0; i < nacct; i++)
        if (strncmp(accts[i].hash, "$tos$", 5) == 0) return 1;
    return 0;
}

const char *auth_hostname(void) { return hostname; }

void auth_set_hostname(const char *name)
{
    copy_str(hostname, name, sizeof(hostname));
    auth_saved_t saved;
    auth_become(0, 0, &saved);
    char line[48];
    strcpy(line, hostname);
    strcat(line, "\n");
    put_file(HOSTNAME_FILE, line, (uint32_t)strlen(line), 0644);
    auth_restore(&saved);
}

/* ---------- lookups ---------- */

int auth_lookup(const char *name, auth_user_t *out)
{
    for (int i = 0; i < nacct; i++)
        if (strcmp(accts[i].u.name, name) == 0) { if (out) *out = accts[i].u; return 0; }
    return -1;
}

int auth_lookup_uid(uint32_t uid, auth_user_t *out)
{
    for (int i = 0; i < nacct; i++)
        if (accts[i].u.uid == uid) { if (out) *out = accts[i].u; return 0; }
    return -1;
}

void sprintf_u32(char *out, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

const char *auth_name_of(uint32_t uid)
{
    static char buf[AUTH_NAME_MAX];
    for (int i = 0; i < nacct; i++)
        if (accts[i].u.uid == uid) return accts[i].u.name;
    sprintf_u32(buf, uid);
    return buf;
}

const char *auth_group_name_of(uint32_t gid)
{
    static char buf[AUTH_NAME_MAX];
    for (int i = 0; i < ngroup; i++)
        if (groups[i].gid == gid) return groups[i].name;
    sprintf_u32(buf, gid);
    return buf;
}

int auth_group_lookup(const char *name, uint32_t *gid)
{
    for (int i = 0; i < ngroup; i++)
        if (strcmp(groups[i].name, name) == 0) { *gid = groups[i].gid; return 0; }
    return -1;
}

static int member_listed(const char *members, const char *name)
{
    size_t n = strlen(name);
    const char *p = members;
    while (*p) {
        const char *e = p;
        while (*e && *e != ',') e++;
        if ((size_t)(e - p) == n && strncmp(p, name, n) == 0) return 1;
        p = *e ? e + 1 : e;
    }
    return 0;
}

int auth_user_in_group(uint32_t uid, const char *group)
{
    auth_user_t u;
    if (auth_lookup_uid(uid, &u) != 0) return 0;
    for (int i = 0; i < ngroup; i++) {
        if (strcmp(groups[i].name, group) != 0) continue;
        if (groups[i].gid == u.gid || member_listed(groups[i].members, u.name)) return 1;
    }
    return 0;
}

int auth_in_gid(uint32_t uid, uint32_t gid)
{
    auth_user_t u;
    if (auth_lookup_uid(uid, &u) != 0) return 0;
    if (u.gid == gid) return 1;
    for (int i = 0; i < ngroup; i++)
        if (groups[i].gid == gid && member_listed(groups[i].members, u.name)) return 1;
    return 0;
}

void auth_mark_confirmed(void) { confirmed = 1; }

int auth_can_sudo(uint32_t uid)
{
    return uid == 0 || auth_user_in_group(uid, AUTH_SUDO_GROUP) || auth_user_in_group(uid, "wheel");
}

/* ---------- passwords and account changes ---------- */

int auth_check_password(const char *name, const char *password)
{
    for (int i = 0; i < nacct; i++)
        if (strcmp(accts[i].u.name, name) == 0) return verify_hash(accts[i].hash, password);
    /* unknown user: still spend the time, so the answer does not leak which names exist */
    char dummy[AUTH_HASH_MAX];
    make_hash(password, dummy);
    return -1;
}

int auth_user_has_password(const char *name)
{
    for (int i = 0; i < nacct; i++)
        if (strcmp(accts[i].u.name, name) == 0) return strncmp(accts[i].hash, "$tos$", 5) == 0;
    return 0;
}

int auth_set_password(const char *name, const char *password)
{
    for (int i = 0; i < nacct; i++) {
        if (strcmp(accts[i].u.name, name) != 0) continue;
        if (password[0]) make_hash(password, accts[i].hash);
        else accts[i].hash[0] = 0;
        return auth_save();
    }
    return -1;
}

int auth_set_root_password(const char *password) { return auth_set_password("root", password); }

static int name_ok(const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n >= AUTH_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok || (i == 0 && c >= '0' && c <= '9') || (i == 0 && c == '-')) return 0;
    }
    return 1;
}

int auth_add_user(const char *name, const char *password, const char *gecos, int admin, uint32_t *uid_out)
{
    if (!name_ok(name) || auth_lookup(name, 0) == 0 || nacct >= AUTH_MAX_USERS || ngroup >= AUTH_MAX_GROUPS) return -1;
    uint32_t uid = AUTH_FIRST_UID;
    for (;;) {
        int taken = 0;
        for (int i = 0; i < nacct; i++) if (accts[i].u.uid == uid) taken = 1;
        for (int i = 0; i < ngroup; i++) if (groups[i].gid == uid) taken = 1;
        if (!taken) break;
        uid++;
    }
    acct_t *a = &accts[nacct++];
    memset(a, 0, sizeof(*a));
    copy_str(a->u.name, name, AUTH_NAME_MAX);
    a->u.uid = uid;
    a->u.gid = uid;                                         /* a private group, as on Linux */
    copy_str(a->u.gecos, gecos ? gecos : name, sizeof(a->u.gecos));
    strcpy(a->u.home, "/home/");
    strcat(a->u.home, name);
    strcpy(a->u.shell, "/system/shell");
    if (password && password[0]) make_hash(password, a->hash); else copy_str(a->hash, "!", AUTH_HASH_MAX);
    group_t *g = &groups[ngroup++];
    memset(g, 0, sizeof(*g));
    copy_str(g->name, name, AUTH_NAME_MAX);
    g->gid = uid;
    if (admin) {
        for (int i = 0; i < ngroup; i++) {
            if (strcmp(groups[i].name, AUTH_SUDO_GROUP) != 0) continue;
            size_t l = strlen(groups[i].members);
            if (l + strlen(name) + 2 < sizeof(groups[i].members)) {
                if (l) strcat(groups[i].members, ",");
                strcat(groups[i].members, name);
            }
        }
    }
    if (auth_save() != 0) return -1;

    auth_saved_t saved;
    auth_become(0, 0, &saved);
    if (!fsbridge_exists("/home")) { fsbridge_mkdir("/home"); vfs_chown("/home", 0, 0); vfs_chmod("/home", 0755); }
    if (!fsbridge_exists(a->u.home)) fsbridge_mkdir(a->u.home);
    vfs_chown(a->u.home, uid, uid);
    vfs_chmod(a->u.home, 0750);
    auth_restore(&saved);
    if (uid_out) *uid_out = uid;
    return 0;
}

int auth_del_user(const char *name)
{
    if (strcmp(name, "root") == 0) return -1;
    for (int i = 0; i < nacct; i++) {
        if (strcmp(accts[i].u.name, name) != 0) continue;
        uint32_t gid = accts[i].u.gid;
        for (int j = i; j < nacct - 1; j++) accts[j] = accts[j + 1];
        nacct--;
        for (int j = 0; j < ngroup; j++) {                  /* its private group, and any membership */
            if (groups[j].gid == gid && strcmp(groups[j].name, name) == 0 && gid >= AUTH_FIRST_UID) {
                for (int k = j; k < ngroup - 1; k++) groups[k] = groups[k + 1];
                ngroup--;
                j--;
                continue;
            }
            char *m = groups[j].members;
            size_t n = strlen(name);
            char *p = m;
            while (*p) {
                char *e = p;
                while (*e && *e != ',') e++;
                if ((size_t)(e - p) == n && strncmp(p, name, n) == 0) {
                    if (*e) memmove(p, e + 1, strlen(e + 1) + 1);
                    else { if (p > m) p[-1] = 0; else *p = 0; }
                    break;
                }
                p = *e ? e + 1 : e;
            }
        }
        return auth_save();
    }
    return -1;
}

/* ---------- prompts ---------- */

int auth_read_line(const char *prompt, char *out, int max, int echo)
{
    terminal_writestring(prompt);
    int i = 0;
    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n' || c == '\r') { terminal_putchar('\n'); out[i] = 0; return i; }
        if (c == 3) { terminal_writestring("^C\n"); out[0] = 0; return -1; }
        if (c == '\b' || c == 127) {
            if (i > 0) { i--; if (echo) { terminal_putchar('\b'); terminal_putchar(' '); terminal_putchar('\b'); } }
            continue;
        }
        if ((unsigned char)c >= ' ' && i < max - 1) {
            out[i++] = c;
            if (echo) terminal_putchar(c);
        }
    }
}

/* the graphical desktop's password dialog (wm_gfx.c), used when a window's task asks */
extern int wm_gfx_mode;
int wm_gfx_password_sheet(const char *prompt, char *out, int max);

int auth_prompt_password(const char *prompt, char *out, int max)
{
    if (wm_gfx_mode && task_get_userdata()) {
        int r = wm_gfx_password_sheet(prompt, out, max);
        if (r != -2) return r;
    }
    return auth_read_line(prompt, out, max, 0);
}

void auth_command_begin(void) { confirmed = 0; }

int auth_confirm_privileged(const char *what)
{
    if (confirmed) return 0;
    const char *user = auth_name_of(auth_uid());           /* the account that is acting (root in a root shell) */
    if (!auth_user_has_password(user)) { confirmed = 1; return 0; }     /* nothing to ask (live session) */
    for (int attempt = 0; attempt < 3; attempt++) {
        char prompt[96], pw[64];
        strcpy(prompt, "[system] password for ");
        strcat(prompt, user);
        strcat(prompt, " to ");
        strcat(prompt, what);
        strcat(prompt, ": ");
        if (auth_prompt_password(prompt, pw, sizeof(pw)) < 0) return -1;
        int ok = auth_check_password(user, pw) == 0;
        memset(pw, 0, sizeof(pw));
        if (ok) { confirmed = 1; return 0; }
        terminal_writestring("Sorry, try again.\n");
    }
    terminal_writestring("3 incorrect password attempts\n");
    return -1;
}
