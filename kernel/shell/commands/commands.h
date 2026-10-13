#ifndef COMMANDS_H
#define COMMANDS_H

#include <stdint.h>

void cmd_help(int argc, char **args);
void cmd_man(int argc, char **args);
void cmd_echo(int argc, char **args);
void cmd_clear(int argc, char **args);
void cmd_pwd(int argc, char **args);
void cmd_ls(int argc, char **args);
void cmd_cd(int argc, char **args);
void cmd_mkdir(int argc, char **args);
void cmd_rmdir(int argc, char **args);
void cmd_rm(int argc, char **args);
void cmd_touch(int argc, char **args);
void cmd_cat(int argc, char **args);
void cmd_mv(int argc, char **args);
void cmd_cp(int argc, char **args);
void cmd_edit(int argc, char **args);
void cmd_reboot(int argc, char **args);
void cmd_shutdown(int argc, char **args);
void cmd_version(int argc, char **args);
void cmd_beep(int argc, char **args);
void cmd_soundinfo(int argc, char **args);
void cmd_vgatest(int argc, char **args);
void cmd_fbtest(int argc, char **args);
void cmd_doom(int argc, char **args);
void cmd_wolf3d(int argc, char **args);
void cmd_3d(int argc, char **args);
void cmd_crash(int argc, char **args);
void cmd_about(int argc, char **args);
void cmd_uname(int argc, char **args);
void cmd_ping(int argc, char **args);
void cmd_ifconfig(int argc, char **args);
void cmd_wget(int argc, char **args);
void cmd_ws(int argc, char **args);
void cmd_tls(int argc, char **args);
void cmd_tpkg(int argc, char **args);
void cmd_sctp_connect(int argc, char **args);
void cmd_sctp_send(int argc, char **args);
void cmd_sctp_close(int argc, char **args);
void cmd_dccp_connect(int argc, char **args);
void cmd_dccp_send(int argc, char **args);
void cmd_udplite_send(int argc, char **args);
void cmd_ping6(int argc, char **args);
void cmd_ip6addr(int argc, char **args);
void cmd_ipsec_sa(int argc, char **args);
void cmd_ipsec(int argc, char **args);
void cmd_vlan(int argc, char **args);
void cmd_bridge(int argc, char **args);
void cmd_bond(int argc, char **args);
void cmd_ipx(int argc, char **args);
void cmd_route(int argc, char **args);
void cmd_policy(int argc, char **args);
void cmd_fw(int argc, char **args);
void cmd_gre(int argc, char **args);
void cmd_ipip(int argc, char **args);
void cmd_wg(int argc, char **args);
void cmd_tsharp(int argc, char **args);
void cmd_python(int argc, char **args);
void cmd_run(int argc, char **args);

void cmd_head(int argc, char **args);
void cmd_tail(int argc, char **args);
void cmd_wc(int argc, char **args);
void cmd_sort(int argc, char **args);
void cmd_grep(int argc, char **args);
void cmd_find(int argc, char **args);
void cmd_rev(int argc, char **args);
void cmd_uniq(int argc, char **args);
void cmd_date(int argc, char **args);
void cmd_whoami(int argc, char **args);
void cmd_hostname(int argc, char **args);
void cmd_cal(int argc, char **args);
void cmd_yes(int argc, char **args);
void cmd_seq(int argc, char **args);
void cmd_sleep(int argc, char **args);
void cmd_df(int argc, char **args);
void cmd_free(int argc, char **args);
void cmd_dmesg(int argc, char **args);
void cmd_basename(int argc, char **args);
void cmd_dirname(int argc, char **args);
void cmd_which(int argc, char **args);
void cmd_env(int argc, char **args);

void cmd_uptime(int argc, char **args);
void cmd_ps(int argc, char **args);
void cmd_log(int argc, char **args);
void cmd_kill(int argc, char **args);
void cmd_chmod(int argc, char **args);
void cmd_id(int argc, char **args);
void cmd_groups(int argc, char **args);
void cmd_passwd(int argc, char **args);
void cmd_useradd(int argc, char **args);
void cmd_userdel(int argc, char **args);
void cmd_chown(int argc, char **args);
void cmd_chgrp(int argc, char **args);
void cmd_umask(int argc, char **args);
void cmd_stat(int argc, char **args);
void cmd_su(int argc, char **args);
void cmd_sudo(int argc, char **args);
void cmd_exit(int argc, char **args);

struct vfs_entry;
void report_fs_failure(const char *cmd, const char *path);   /* "cmd: path: Permission denied" or "Failed" */
void format_mode(uint32_t mode, int is_dir, char *out);       /* drwxr-xr-x */
void ls_long_line(const struct vfs_entry *e);
void cmd_hexdump(int argc, char **args);
void cmd_tee(int argc, char **args);
void cmd_alias(int argc, char **args);
void cmd_history(int argc, char **args);
void cmd_unalias(int argc, char **args);
void cmd_font(int argc, char **args);
void cmd_htop(int argc, char **args);
void cmd_disk(int argc, char **args);
void cmd_tar(int argc, char **args);
void cmd_zip(int argc, char **args);
void cmd_unzip(int argc, char **args);

#endif
