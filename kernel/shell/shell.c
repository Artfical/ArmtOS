#include "shell.h"
#include "terminal.h"
#include "keyboard.h"
#include "gui.h"
#include "string.h"
#include "version.h"
#include "ramfs.h"
#include "commands.h"
#include "vfs.h"
#include "scheduler.h"
#include "micropython.h"
#include "fsbridge.h"
#include "memory.h"
#include "auth.h"
#include "perm.h"

#define PATH_FILE "/system/path.tmbl"

/* Runs whatever `fullpath` is, dispatching on its extension --
 * shared by the "cmdname=fullpath" lookup below and the legacy
 * fallback. Returns 1 if it recognized the extension and ran it. */
static int path_run_file(const char *fullpath, int argc, char **args)
{
    int len = strlen(fullpath);
    if (len >= 3 && strcmp(fullpath + len - 3, ".py") == 0) {
        micropython_run_file_argv(fullpath, argc, args);
        return 1;
    }
    if (len >= 2 && strcmp(fullpath + len - 2, ".t") == 0) {
        char *run_args[2];
        run_args[0] = "run";
        run_args[1] = (char *)fullpath;
        cmd_run(2, run_args);
        return 1;
    }
    return 0;
}

/* Resolves an unrecognized command against /sys/path.tmbl -- plain
 * text, one "cmdname=/full/path/to/file.py-or-.t" mapping per line.
 * A package's own tpkg_path_register() (cmd_tpkg.c) writes these:
 * one line per file that ships with a "<file>.txt" sidecar declaring
 * its command name (so the invokable name doesn't have to match the
 * file's own basename, and one package can expose several commands),
 * or a single default line matching the package name for packages
 * that don't use sidecars. Also falls back to the original hardcoded
 * /programs/<cmd>/<cmd>.py convention for anything installed before
 * path.tmbl existed. Returns 1 if something ran. */
static int path_fallback_exec(const char *cmd, int argc, char **args)
{
    if (ramfs_exists(PATH_FILE)) {
        uint32_t size = ramfs_size(PATH_FILE);
        char *buf = (char *)malloc(size + 1);
        if (buf) {
            int n = ramfs_read(PATH_FILE, buf, size, 0);
            if (n > 0) {
                buf[n] = 0;
                int cmd_len = strlen(cmd);
                char *p = buf;
                while (*p) {
                    char *line_start = p;
                    while (*p && *p != '\n') p++;
                    int line_len = (int)(p - line_start);
                    if (*p == '\n') { *p = 0; p++; }
                    char *eq = NULL;
                    for (int i = 0; i < line_len; i++) {
                        if (line_start[i] == '=') { eq = line_start + i; break; }
                    }
                    if (eq && (eq - line_start) == cmd_len &&
                        strncmp(line_start, cmd, cmd_len) == 0) {
                        int ran = path_run_file(eq + 1, argc, args);
                        free(buf);
                        return ran;
                    }
                }
            }
            free(buf);
        }
    }

    /* the applications in /system/apps, the system's own programs, then the legacy initrd location */
    char path[160];
    strcpy(path, "/system/apps/");
    strcat(path, cmd);
    strcat(path, ".t");
    if (ramfs_exists(path)) {
        path_run_file(path, argc, args);
        return 1;
    }
    strcpy(path, "/system/programs/");
    strcat(path, cmd);
    strcat(path, "/");
    strcat(path, cmd);
    strcat(path, ".py");
    if (ramfs_exists(path)) {
        micropython_run_file_argv(path, argc, args);
        return 1;
    }
    strcpy(path, "/programs/");
    strcat(path, cmd);
    strcat(path, "/");
    strcat(path, cmd);
    strcat(path, ".py");
    if (ramfs_exists(path)) {
        micropython_run_file_argv(path, argc, args);
        return 1;
    }
    return 0;
}

#define MAX_ARGS 16
#define MAX_CMD_LEN 512
#define HIST_SIZE 16
#define MAX_ALIASES 16

static char history[HIST_SIZE][MAX_CMD_LEN];
static int hist_count = 0;
static int hist_pos = -1;
static int hist_current = -1;

typedef struct {
    char name[32];
    char value[MAX_CMD_LEN];
} alias_t;

static alias_t aliases[MAX_ALIASES];
static int alias_count = 0;

static void history_add(const char *cmd)
{
    if (!cmd[0]) return;
    if (hist_count > 0 && strcmp(history[(hist_count - 1) % HIST_SIZE], cmd) == 0)
        return;
    int idx = hist_count % HIST_SIZE;
    int i = 0;
    while (cmd[i] && i < MAX_CMD_LEN - 1) { history[idx][i] = cmd[i]; i++; }
    history[idx][i] = 0;
    hist_count++;
    hist_pos = hist_count;
    hist_current = -1;
}

static const char *history_get(int offset)
{
    if (hist_count == 0) return NULL;
    int idx = offset % HIST_SIZE;
    return history[idx];
}

/* Splits on plain spaces like before, but a '"' or '\'' opens a quoted
 * token that runs (spaces included) until the matching close quote --
 * needed for anything that takes a multi-word argument, e.g.
 * `git commit -m "fix thing"`. The quotes themselves are stripped by
 * shifting the token left in place, since args[] just points into the
 * same cmd buffer being tokenized. */
static int parse_args(char *cmd, char **args)
{
    int argc = 0;
    char *p = cmd;
    while (*p == ' ') p++;
    while (*p && argc < MAX_ARGS) {
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            p++;
            char *tok = p;
            char *w = p;
            while (*p && *p != quote) *w++ = *p++;
            if (*p == quote) p++;
            *w = '\0';
            args[argc++] = tok;
        } else {
            args[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) { *p = '\0'; p++; }
        }
        while (*p == ' ') p++;
    }
    return argc;
}

static const char *builtin_names[] = {
    "help", "man", "echo", "clear", "pwd", "ls", "cd", "mkdir", "rmdir", "rm",
    "touch", "cat", "mv", "cp", "edit", "reboot", "shutdown",
    "version", "about", "uname", "ping", "ifconfig", "wget", "ws", "tls", "ipsec", "tpkg", "python", "tsharp", "run",
    "head", "tail", "wc", "sort", "grep", "find", "date", "whoami",
    "hostname", "cal", "yes", "seq", "sleep", "df", "free", "dmesg",
    "basename", "dirname", "which", "env", "uptime", "ps", "log", "kill", "notify",
    "chmod", "hexdump", "tee", "alias", "history", "font", "htop",
    "disk", "rev", "uniq", "tar", "zip", "unzip",
    "id", "groups", "passwd", "useradd", "userdel", "chown", "chgrp", "umask", "stat",
    "su", "sudo", "exit", "logout",
    NULL
};

static int tab_complete(char *buf, int *pos, int max)
{
    (void)max;
    int i = *pos;
    while (i > 0 && buf[i - 1] != ' ') i--;
    int word_len = *pos - i;
    char word[256];
    for (int j = 0; j < word_len && j < 255; j++) word[j] = buf[i + j];
    word[word_len] = 0;

    int first = (i == 0) ? 1 : 0;
    char matches[64][256];
    int match_count = 0;

    if (first || word_len == 0) {
        for (int b = 0; builtin_names[b]; b++) {
            if (word_len == 0 || strncmp(builtin_names[b], word, word_len) == 0) {
                int k = 0;
                while (builtin_names[b][k] && k < 255) { matches[match_count][k] = builtin_names[b][k]; k++; }
                matches[match_count][k] = 0;
                match_count++;
                if (match_count >= 64) break;
            }
        }
    }

    const char *dir = ".";
    if (!first) {
        for (int j = i - 2; j >= 0; j--) {
            if (buf[j] == ' ') { dir = buf + j + 1; break; }
            if (j == 0) dir = buf;
        }
    }

    vfs_entry_t entries[128];
    int n = ramfs_list(dir, entries, 128);
    for (int ei = 0; ei < n && match_count < 64; ei++) {
        if (word_len == 0 || strncmp(entries[ei].name, word, word_len) == 0) {
            if (strcmp(entries[ei].name, ".") != 0 && strcmp(entries[ei].name, "..") != 0) {
                int k = 0;
                while (entries[ei].name[k] && k < 255) { matches[match_count][k] = entries[ei].name[k]; k++; }
                if (entries[ei].is_dir && k < 254) { matches[match_count][k] = '/'; k++; }
                matches[match_count][k] = 0;
                match_count++;
            }
        }
    }

    if (match_count == 0) return 0;
    if (match_count == 1) {
        int k = 0;
        while (matches[0][k]) { buf[i + k] = matches[0][k]; k++; }
        buf[i + k] = ' ';
        *pos = i + k + 1;
        terminal_writestring(matches[0]);
        terminal_putchar(' ');
        return 1;
    }

    terminal_putchar('\n');
    for (int m = 0; m < match_count; m++) {
        terminal_writestring(matches[m]);
        terminal_putchar(' ');
        if ((m + 1) % 5 == 0) terminal_putchar('\n');
    }
    terminal_putchar('\n');
    terminal_writestring(ramfs_getcwd());
    terminal_writestring("> ");
    for (int j = 0; j < *pos; j++) terminal_putchar(buf[j]);
    return 1;
}

static void shell_readline(char *buf, int max)
{
    int i = 0;
    buf[0] = 0;
    for (;;) {
        gui_poll();
        int spec = keyboard_get_special();
        if (spec == 3) {
            if (hist_current < 0) hist_current = hist_count;
            if (hist_current > 0) {
                hist_current--;
                for (int j = 0; j < i; j++) { terminal_putchar('\b'); terminal_putchar(' '); terminal_putchar('\b'); }
                const char *h = history_get(hist_current);
                i = 0;
                if (h) {
                    while (h[i] && i < max - 1) { buf[i] = h[i]; i++; }
                    buf[i] = 0;
                    terminal_writestring(buf);
                }
            }
            continue;
        }
        if (spec == 4) {
            if (hist_current >= 0 && hist_current < hist_count - 1) {
                hist_current++;
                for (int j = 0; j < i; j++) { terminal_putchar('\b'); terminal_putchar(' '); terminal_putchar('\b'); }
                const char *h = history_get(hist_current);
                i = 0;
                if (h) {
                    while (h[i] && i < max - 1) { buf[i] = h[i]; i++; }
                    buf[i] = 0;
                    terminal_writestring(buf);
                }
            } else {
                hist_current = hist_count;
                for (int j = 0; j < i; j++) { terminal_putchar('\b'); terminal_putchar(' '); terminal_putchar('\b'); }
                i = 0;
                buf[0] = 0;
            }
            continue;
        }

        /* Don't call the blocking keyboard_getchar() here: it only
         * watches the regular character queue, not the separate
         * special-key (arrow) queue checked above, so it would block
         * forever waiting for a printable key and never notice an
         * arrow press that arrived in the meantime — history recall
         * would only "wake up" once another regular key was typed.
         * Polling keyboard_data_available() first keeps every loop
         * iteration short enough to re-check keyboard_get_special(). */
        if (!keyboard_data_available()) { task_yield(); continue; }

        char c = keyboard_getchar();
        if (c == '\n') {
            terminal_putchar('\n');
            buf[i] = '\0';
            history_add(buf);
            return;
        } else if (c == '\t') {
            tab_complete(buf, &i, max);
        } else if (c == '\b') {
            if (i > 0) {
                i--;
                terminal_putchar('\b');
                terminal_putchar(' ');
                terminal_putchar('\b');
            }
        } else if (c == 127) {
            if (i > 0) {
                i--;
                terminal_putchar('\b');
                terminal_putchar(' ');
                terminal_putchar('\b');
            }
        } else if (c == 3) {
            terminal_writestring("^C\n");
            buf[0] = '\0';
            return;
        } else if ((unsigned char)c >= ' ' && i < max - 1) {
            buf[i++] = c;
            terminal_putchar(c);
        }
    }
}

static char *expand_alias(const char *cmd)
{
    for (int i = 0; i < alias_count; i++) {
        int alen = 0;
        while (aliases[i].name[alen]) alen++;
        if (strncmp(cmd, aliases[i].name, alen) == 0 &&
            (cmd[alen] == ' ' || cmd[alen] == '\0' || cmd[alen] == '\t')) {
            static char expanded[MAX_CMD_LEN];
            int k = 0;
            int v = 0;
            while (aliases[i].value[v] && k < MAX_CMD_LEN - 1)
                expanded[k++] = aliases[i].value[v++];
            int c = alen;
            if (cmd[c] == ' ') {
                expanded[k++] = ' ';
                c++;
                while (cmd[c] && k < MAX_CMD_LEN - 1)
                    expanded[k++] = cmd[c++];
            }
            expanded[k] = 0;
            return expanded;
        }
    }
    return (char *)cmd;
}

static int alias_add(const char *name, const char *value)
{
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            int k = 0;
            while (value[k] && k < MAX_CMD_LEN - 1) { aliases[i].value[k] = value[k]; k++; }
            aliases[i].value[k] = 0;
            return 0;
        }
    }
    if (alias_count >= MAX_ALIASES) return -1;
    int k = 0;
    while (name[k] && k < 31) { aliases[alias_count].name[k] = name[k]; k++; }
    aliases[alias_count].name[k] = 0;
    k = 0;
    while (value[k] && k < MAX_CMD_LEN - 1) { aliases[alias_count].value[k] = value[k]; k++; }
    aliases[alias_count].value[k] = 0;
    alias_count++;
    return 0;
}

int shell_alias_set(const char *name, const char *value)
{
    return alias_add(name, value);
}

int shell_alias_unset(const char *name)
{
    for (int i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) {
            for (int j = i; j < alias_count - 1; j++) aliases[j] = aliases[j + 1];
            alias_count--;
            return 0;
        }
    }
    return -1;
}

void shell_alias_list(void)
{
    for (int i = 0; i < alias_count; i++) {
        terminal_writestring(aliases[i].name);
        terminal_writestring("=");
        terminal_writestring(aliases[i].value);
        terminal_putchar('\n');
    }
}

void shell_history_show(void)
{
    int start = 0;
    if (hist_count > HIST_SIZE) start = hist_count - HIST_SIZE;
    for (int i = start; i < hist_count; i++) {
        int idx = i % HIST_SIZE;
        char buf[4];
        int n = i + 1;
        int d = 0;
        if (n >= 1000) { buf[d++] = '0' + (n / 1000); n %= 1000; }
        if (d > 0 || n >= 100) { buf[d++] = '0' + (n / 100); n %= 100; }
        if (d > 0 || n >= 10) { buf[d++] = '0' + (n / 10); n %= 10; }
        buf[d++] = '0' + n;
        buf[d++] = ' ';
        buf[d] = 0;
        terminal_writestring(buf);
        terminal_writestring(history[idx]);
        terminal_putchar('\n');
    }
}

const char **shell_builtin_names(void)
{
    return builtin_names;
}

void shell_init(void)
{
    terminal_writestring(TOS_WELCOME_STRING);
    terminal_writestring("Type 'help' for commands\n\n");
}

void shell_dispatch(int argc, char **args);

static void shell_exec_line(char *cmd_line)
{
    char *args[MAX_ARGS];

    char *expanded = expand_alias(cmd_line);
    if (expanded != cmd_line) {
        int ei = 0;
        while (expanded[ei]) { cmd_line[ei] = expanded[ei]; ei++; }
        cmd_line[ei] = 0;
    }

    int argc = parse_args(cmd_line, args);

    if (argc == 0) return;
    auth_command_begin();
    shell_dispatch(argc, args);
}

/* Runs one already-split command (also used by sudo for its argument list). */
void shell_dispatch(int argc, char **args)
{
    const char *c = args[0];

        if (strcmp(c, "help") == 0) {
            cmd_help(argc, args);
        } else if (strcmp(c, "man") == 0) {
            cmd_man(argc, args);
        } else if (strcmp(c, "echo") == 0) {
            cmd_echo(argc, args);
        } else if (strcmp(c, "clear") == 0) {
            cmd_clear(argc, args);
        } else if (strcmp(c, "pwd") == 0) {
            cmd_pwd(argc, args);
        } else if (strcmp(c, "ls") == 0) {
            cmd_ls(argc, args);
        } else if (strcmp(c, "cd") == 0) {
            cmd_cd(argc, args);
        } else if (strcmp(c, "mkdir") == 0) {
            cmd_mkdir(argc, args);
        } else if (strcmp(c, "rmdir") == 0) {
            cmd_rmdir(argc, args);
        } else if (strcmp(c, "rm") == 0) {
            cmd_rm(argc, args);
        } else if (strcmp(c, "touch") == 0) {
            cmd_touch(argc, args);
        } else if (strcmp(c, "cat") == 0) {
            cmd_cat(argc, args);
        } else if (strcmp(c, "head") == 0) {
            cmd_head(argc, args);
        } else if (strcmp(c, "tail") == 0) {
            cmd_tail(argc, args);
        } else if (strcmp(c, "wc") == 0) {
            cmd_wc(argc, args);
        } else if (strcmp(c, "sort") == 0) {
            cmd_sort(argc, args);
        } else if (strcmp(c, "grep") == 0) {
            cmd_grep(argc, args);
        } else if (strcmp(c, "mv") == 0) {
            cmd_mv(argc, args);
        } else if (strcmp(c, "cp") == 0) {
            cmd_cp(argc, args);
        } else if (strcmp(c, "find") == 0) {
            cmd_find(argc, args);
        } else if (strcmp(c, "rev") == 0) {
            cmd_rev(argc, args);
        } else if (strcmp(c, "uniq") == 0) {
            cmd_uniq(argc, args);
        } else if (strcmp(c, "edit") == 0) {
            cmd_edit(argc, args);
        } else if (strcmp(c, "tsharp") == 0) {
            cmd_tsharp(argc, args);
        } else if (strcmp(c, "python") == 0) {
            cmd_python(argc, args);
        } else if (strcmp(c, "run") == 0) {
            cmd_run(argc, args);
        } else if (strcmp(c, "reboot") == 0) {
            cmd_reboot(argc, args);
        } else if (strcmp(c, "shutdown") == 0) {
            cmd_shutdown(argc, args);
        } else if (strcmp(c, "version") == 0) {
            cmd_version(argc, args);
        } else if (strcmp(c, "beep") == 0) {
            cmd_beep(argc, args);
        } else if (strcmp(c, "soundinfo") == 0) {
            cmd_soundinfo(argc, args);
        } else if (strcmp(c, "vgatest") == 0) {
            cmd_vgatest(argc, args);
        } else if (strcmp(c, "fbtest") == 0) {
            cmd_fbtest(argc, args);
        } else if (strcmp(c, "doom") == 0) {
            cmd_doom(argc, args);
        } else if (strcmp(c, "wolf3d") == 0) {
            cmd_wolf3d(argc, args);
        } else if (strcmp(c, "3d") == 0) {
            cmd_3d(argc, args);
        } else if (strcmp(c, "crash") == 0) {
            cmd_crash(argc, args);
        } else if (strcmp(c, "about") == 0) {
            cmd_about(argc, args);
        } else if (strcmp(c, "uname") == 0) {
            cmd_uname(argc, args);
        } else if (strcmp(c, "whoami") == 0) {
            cmd_whoami(argc, args);
        } else if (strcmp(c, "hostname") == 0) {
            cmd_hostname(argc, args);
        } else if (strcmp(c, "date") == 0) {
            cmd_date(argc, args);
        } else if (strcmp(c, "cal") == 0) {
            cmd_cal(argc, args);
        } else if (strcmp(c, "df") == 0) {
            cmd_df(argc, args);
        } else if (strcmp(c, "free") == 0) {
            cmd_free(argc, args);
        } else if (strcmp(c, "dmesg") == 0) {
            cmd_dmesg(argc, args);
        } else if (strcmp(c, "yes") == 0) {
            cmd_yes(argc, args);
        } else if (strcmp(c, "seq") == 0) {
            cmd_seq(argc, args);
        } else if (strcmp(c, "sleep") == 0) {
            cmd_sleep(argc, args);
        } else if (strcmp(c, "basename") == 0) {
            cmd_basename(argc, args);
        } else if (strcmp(c, "dirname") == 0) {
            cmd_dirname(argc, args);
        } else if (strcmp(c, "which") == 0) {
            cmd_which(argc, args);
        } else if (strcmp(c, "env") == 0) {
            cmd_env(argc, args);
        } else if (strcmp(c, "ping") == 0) {
            cmd_ping(argc, args);
        } else if (strcmp(c, "ifconfig") == 0) {
            cmd_ifconfig(argc, args);
        } else if (strcmp(c, "ping6") == 0) {
            cmd_ping6(argc, args);
        } else if (strcmp(c, "ip6addr") == 0) {
            cmd_ip6addr(argc, args);
        } else if (strcmp(c, "sctp_connect") == 0) {
            cmd_sctp_connect(argc, args);
        } else if (strcmp(c, "sctp_send") == 0) {
            cmd_sctp_send(argc, args);
        } else if (strcmp(c, "sctp_close") == 0) {
            cmd_sctp_close(argc, args);
        } else if (strcmp(c, "dccp_connect") == 0) {
            cmd_dccp_connect(argc, args);
        } else if (strcmp(c, "dccp_send") == 0) {
            cmd_dccp_send(argc, args);
        } else if (strcmp(c, "udplite_send") == 0) {
            cmd_udplite_send(argc, args);
        } else if (strcmp(c, "ipsec_sa") == 0) {
            cmd_ipsec_sa(argc, args);
        } else if (strcmp(c, "ipsec") == 0) {
            cmd_ipsec(argc, args);
        } else if (strcmp(c, "vlan") == 0) {
            cmd_vlan(argc, args);
        } else if (strcmp(c, "bridge") == 0) {
            cmd_bridge(argc, args);
        } else if (strcmp(c, "bond") == 0) {
            cmd_bond(argc, args);
        } else if (strcmp(c, "ipx") == 0) {
            cmd_ipx(argc, args);
        } else if (strcmp(c, "route") == 0) {
            cmd_route(argc, args);
        } else if (strcmp(c, "policy") == 0) {
            cmd_policy(argc, args);
        } else if (strcmp(c, "fw") == 0) {
            cmd_fw(argc, args);
        } else if (strcmp(c, "gre") == 0) {
            cmd_gre(argc, args);
        } else if (strcmp(c, "ipip") == 0) {
            cmd_ipip(argc, args);
        } else if (strcmp(c, "wg") == 0) {
            cmd_wg(argc, args);
        } else if (strcmp(c, "wget") == 0) {
            cmd_wget(argc, args);
        } else if (strcmp(c, "ws") == 0) {
            cmd_ws(argc, args);
        } else if (strcmp(c, "tls") == 0) {
            cmd_tls(argc, args);
        } else if (strcmp(c, "tpkg") == 0) {
            cmd_tpkg(argc, args);
        } else if (strcmp(c, "uptime") == 0) {
            cmd_uptime(argc, args);
        } else if (strcmp(c, "notify") == 0) {
            cmd_notify(argc, args);
        } else if (strcmp(c, "ps") == 0) {
            cmd_ps(argc, args);
        } else if (strcmp(c, "log") == 0) {
            cmd_log(argc, args);
        } else if (strcmp(c, "kill") == 0) {
            cmd_kill(argc, args);
        } else if (strcmp(c, "chmod") == 0) {
            cmd_chmod(argc, args);
        } else if (strcmp(c, "hexdump") == 0) {
            cmd_hexdump(argc, args);
        } else if (strcmp(c, "tee") == 0) {
            cmd_tee(argc, args);
        } else if (strcmp(c, "alias") == 0) {
            cmd_alias(argc, args);
        } else if (strcmp(c, "history") == 0) {
            cmd_history(argc, args);
        } else if (strcmp(c, "unalias") == 0) {
            cmd_unalias(argc, args);
        } else if (strcmp(c, "font") == 0) {
            cmd_font(argc, args);
        } else if (strcmp(c, "htop") == 0) {
            cmd_htop(argc, args);
        } else if (strcmp(c, "disk") == 0) {
            cmd_disk(argc, args);
        } else if (strcmp(c, "tar") == 0) {
            cmd_tar(argc, args);
        } else if (strcmp(c, "zip") == 0) {
            cmd_zip(argc, args);
        } else if (strcmp(c, "unzip") == 0) {
            cmd_unzip(argc, args);
        } else if (strcmp(c, "id") == 0) {
            cmd_id(argc, args);
        } else if (strcmp(c, "groups") == 0) {
            cmd_groups(argc, args);
        } else if (strcmp(c, "passwd") == 0) {
            cmd_passwd(argc, args);
        } else if (strcmp(c, "useradd") == 0) {
            cmd_useradd(argc, args);
        } else if (strcmp(c, "userdel") == 0) {
            cmd_userdel(argc, args);
        } else if (strcmp(c, "chown") == 0) {
            cmd_chown(argc, args);
        } else if (strcmp(c, "chgrp") == 0) {
            cmd_chgrp(argc, args);
        } else if (strcmp(c, "umask") == 0) {
            cmd_umask(argc, args);
        } else if (strcmp(c, "stat") == 0) {
            cmd_stat(argc, args);
        } else if (strcmp(c, "su") == 0) {
            cmd_su(argc, args);
        } else if (strcmp(c, "sudo") == 0) {
            cmd_sudo(argc, args);
        } else if (strcmp(c, "exit") == 0 || strcmp(c, "logout") == 0) {
            cmd_exit(argc, args);
        } else {
            if (!path_fallback_exec(c, argc, args)) {
                terminal_writestring("Unknown command: ");
                terminal_writestring(c);
                terminal_putchar('\n');
            }
        }
}

/* Runs a single command line with its output captured into `out`
 * instead of going to the screen, for scripting APIs (T#, MicroPython)
 * that want a command's text back rather than its side effects on the
 * visible terminal. shell_exec_line() mutates its argument in place
 * (alias expansion, tokenizing), so it gets its own writable copy. */
void shell_exec_capture(const char *cmd, char *out, int max)
{
    char buf[MAX_CMD_LEN];
    int i = 0;
    while (cmd[i] && i < MAX_CMD_LEN - 1) { buf[i] = cmd[i]; i++; }
    buf[i] = 0;

    terminal_capture_start(out, max);
    shell_exec_line(buf);
    terminal_capture_stop();
}

static int exit_requested;
static int in_window;

void shell_request_exit(void)
{
    if (in_window) {
        terminal_writestring("(close the window to leave this terminal)\n");
        return;
    }
    exit_requested = 1;
}

static void print_prompt(void)
{
    uint32_t uid = auth_uid();
    const char *cwd = ramfs_getcwd();
    auth_user_t me;
    terminal_writestring(auth_name_of(uid));
    terminal_putchar('@');
    terminal_writestring(auth_hostname());
    terminal_putchar(':');
    if (auth_lookup_uid(uid, &me) == 0 && me.home[0] && strncmp(cwd, me.home, strlen(me.home)) == 0 &&
        (cwd[strlen(me.home)] == 0 || cwd[strlen(me.home)] == '/')) {
        terminal_putchar('~');
        terminal_writestring(cwd + strlen(me.home));
    } else {
        terminal_writestring(cwd);
    }
    terminal_writestring(uid == 0 ? "# " : "$ ");
}

/* commands until `exit`: the login shell, or a nested one started by su / sudo -i */
static void session_loop(void)
{
    char cmd_line[MAX_CMD_LEN];
    exit_requested = 0;
    while (!exit_requested) {
        gui_poll();
        print_prompt();
        shell_readline(cmd_line, MAX_CMD_LEN);
        shell_exec_line(cmd_line);
    }
    exit_requested = 0;
}

void shell_subshell(void)
{
    session_loop();
}

/* Asks who is there. A system without accounts (booted live, nothing installed) lets
 * root in directly; an installed one wants a name and a password. */
void shell_login(void)
{
    auth_init();
    if (!auth_have_accounts()) {
        auth_set_session(0, 0);
        ramfs_chdir("/");
        terminal_writestring("\nLive session: no accounts are set up, you are root.\n");
        return;
    }
    auth_set_session(0, 0);
    for (;;) {
        char name[AUTH_NAME_MAX], pw[64];
        terminal_writestring("\n");
        terminal_writestring(auth_hostname());
        if (auth_read_line(" login: ", name, sizeof(name), 1) < 0 || name[0] == 0) continue;
        if (auth_prompt_password("Password: ", pw, sizeof(pw)) < 0) continue;
        int bad = auth_check_password(name, pw) != 0;
        memset(pw, 0, sizeof(pw));
        auth_user_t u;
        if (bad || auth_lookup(name, &u) != 0) {
            task_sleep(1500);                       /* slow down guessing */
            terminal_writestring("Login incorrect\n");
            continue;
        }
        auth_set_session(u.uid, u.gid);
        if (!fsbridge_exists(u.home) || ramfs_chdir(u.home) != 0) ramfs_chdir("/");
        terminal_writestring("\nWelcome, ");
        terminal_writestring(u.name);
        terminal_writestring("\n");
        return;
    }
}

void shell_run(void)
{
    for (;;) {
        session_loop();
        terminal_writestring("logout\n");
        shell_login();
    }
}

void shell_run_windowed(const char *initial_cmd)
{
    char cmd_line[MAX_CMD_LEN];
    in_window = 1;

    if (initial_cmd && initial_cmd[0]) {
        print_prompt();
        terminal_writestring(initial_cmd);
        terminal_putchar('\n');
        int i = 0;
        while (initial_cmd[i] && i < MAX_CMD_LEN - 1) { cmd_line[i] = initial_cmd[i]; i++; }
        cmd_line[i] = 0;
        shell_exec_line(cmd_line);
    }

    for (;;) {
        print_prompt();

        shell_readline(cmd_line, MAX_CMD_LEN);
        shell_exec_line(cmd_line);
    }
}
