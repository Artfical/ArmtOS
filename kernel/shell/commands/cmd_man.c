/* "man" — long-form documentation for every shell command, plus the
 * scripting API (tos_api as seen from MicroPython's `tos` module and
 * from T#'s built-in functions) and the `tosgui` GUI module. Each
 * page is a short prose description followed by at least one
 * runnable example, since a one-line summary (see cmd_help) isn't
 * enough to actually learn a command from. */

#include "commands.h"
#include "terminal.h"
#include "string.h"

typedef struct {
    const char *name;
    const char *body;
} man_page_t;

static const man_page_t man_pages[] = {
{"help",
"help - list every shell command with a one-line summary\n\n"
"Prints the short command table. Use 'man <command>' for the long\n"
"explanation and examples of any single command, or 'man' alone for\n"
"the full list of man pages (including scripting topics).\n\n"
"Example:\n"
"  /> help\n"
"  /> man cat\n"
},
{"man",
"man - show the long-form manual page for a command or topic\n\n"
"Usage: man <name>\n"
"With no argument, lists every available page (commands and\n"
"scripting topics). Topics beyond plain shell commands include\n"
"'tos_api' (the C API shared by MicroPython and T#), 'tsharp' (T#\n"
"built-in functions), and 'tosgui' (the MicroPython GUI module).\n\n"
"Examples:\n"
"  /> man\n"
"  /> man ls\n"
"  /> man tosgui\n"
},
{"echo",
"echo - print text to the terminal\n\n"
"Prints all of its arguments, separated by single spaces, followed\n"
"by a newline. Useful on its own or piped/redirected with 'tee'.\n\n"
"Examples:\n"
"  /> echo hello world\n"
"  hello world\n"
"  /> echo merhaba > /tmp/note.txt\n"
},
{"clear",
"clear - clear the terminal screen\n\n"
"Erases all visible text and moves the cursor back to the top-left.\n"
"It does not affect command history or any open files.\n\n"
"Example:\n"
"  /> clear\n"
},
{"pwd",
"pwd - print the current working directory\n\n"
"Shows the absolute path the shell is currently 'in'; 'cd' changes\n"
"it and relative paths (ones not starting with '/') are resolved\n"
"against it.\n\n"
"Example:\n"
"  /> pwd\n"
"  /\n"
},
{"ls",
"ls - list directory contents\n\n"
"With no argument, lists the current directory. With a path\n"
"argument, lists that directory instead.\n"
"  -l   long format: mode, owner, group, size (drwxr-xr-x root root ...)\n"
"  -a   also show . and ..\n\n"
"Examples:\n"
"  $ ls\n"
"  $ ls -l /system\n"
},
{"cd",
"cd - change the current working directory\n\n"
"Usage: cd <path>\n"
"Accepts absolute paths ('/system') or relative ones ('..',\n"
"'subdir'). You need search (x) permission on every directory on\n"
"the way. You start in your home directory, /home/<name>.\n\n"
"Examples:\n"
"  /> cd /programs\n"
"  /> cd ..\n"
},
{"mkdir",
"mkdir - create a new directory\n\n"
"Usage: mkdir <path>\n"
"Creates a single directory; the parent directory must already\n"
"exist.\n\n"
"Example:\n"
"  /> mkdir /notes\n"
},
{"rmdir",
"rmdir - remove an empty directory\n\n"
"Usage: rmdir <path>\n"
"Fails if the directory still contains files or subdirectories;\n"
"remove those first (or use 'rm' on each entry).\n\n"
"Example:\n"
"  /> rmdir /notes\n"
},
{"rm",
"rm - remove a file\n\n"
"Usage: rm <path>\n"
"Deletes a single file permanently; there is no trash/undo.\n\n"
"Example:\n"
"  /> rm /notes/draft.txt\n"
},
{"touch",
"touch - create an empty file\n\n"
"Usage: touch <path>\n"
"Creates a new, empty file at the given path. If the file already\n"
"exists, it is left unchanged.\n\n"
"Example:\n"
"  /> touch /notes/todo.txt\n"
},
{"cat",
"cat - print the contents of a file\n\n"
"Usage: cat <path>\n"
"Reads the whole file and writes it to the terminal as-is.\n\n"
"Example:\n"
"  /> cat /notes/todo.txt\n"
},
{"head",
"head - print the first lines of a file\n\n"
"Usage: head <path> [n]\n"
"Prints the first 10 lines by default, or the first 'n' lines if\n"
"given.\n\n"
"Examples:\n"
"  /> head /notes/todo.txt\n"
"  /> head /notes/todo.txt 3\n"
},
{"tail",
"tail - print the last lines of a file\n\n"
"Usage: tail <path> [n]\n"
"Prints the last 10 lines by default, or the last 'n' lines if\n"
"given.\n\n"
"Examples:\n"
"  /> tail /notes/todo.txt\n"
"  /> tail /notes/todo.txt 5\n"
},
{"wc",
"wc - count lines, words, and characters in a file\n\n"
"Usage: wc <path>\n"
"Prints three counts: lines, words, characters.\n\n"
"Example:\n"
"  /> wc /notes/todo.txt\n"
},
{"sort",
"sort - sort the lines of a file alphabetically\n\n"
"Usage: sort <path>\n"
"Reads the file, sorts its lines, and prints the result; the\n"
"original file is not modified.\n\n"
"Example:\n"
"  /> sort /notes/todo.txt\n"
},
{"grep",
"grep - search for a pattern inside a file\n\n"
"Usage: grep <pattern> <path>\n"
"Prints every line of the file that contains the literal substring\n"
"'pattern'. There is no regex support, only plain substring match.\n\n"
"Example:\n"
"  /> grep buy /notes/todo.txt\n"
},
{"mv",
"mv - move or rename a file\n\n"
"Usage: mv <src> <dst>\n"
"Renames a file in place, or moves it to a different directory if\n"
"'dst' is a path in another directory.\n\n"
"Examples:\n"
"  /> mv /notes/todo.txt /notes/done.txt\n"
"  /> mv /notes/done.txt /archive/done.txt\n"
},
{"cp",
"cp - copy a file\n\n"
"Usage: cp <src> <dst>\n"
"Copies the file's contents to a new path, leaving the original in\n"
"place.\n\n"
"Example:\n"
"  /> cp /notes/todo.txt /notes/todo.bak.txt\n"
},
{"find",
"find - search a directory tree for files by name\n\n"
"Usage: find <dir> <name>\n"
"Recursively walks 'dir' and prints the full path of every entry\n"
"whose name matches 'name' exactly.\n\n"
"Example:\n"
"  /> find / todo.txt\n"
},
{"rev",
"rev - reverse the characters of each line\n\n"
"Usage: rev <path>\n"
"Prints the file with every line's characters in reverse order\n"
"(line order itself is unchanged).\n\n"
"Example:\n"
"  /> rev /notes/todo.txt\n"
},
{"uniq",
"uniq - remove adjacent duplicate lines\n\n"
"Usage: uniq <path>\n"
"Like 'sort' but for de-duplication: only collapses runs of\n"
"identical *adjacent* lines, so sort first if the file isn't\n"
"already grouped.\n\n"
"Example:\n"
"  /> sort /notes/todo.txt\n"
"  /> uniq /notes/todo.txt\n"
},
{"edit",
"edit - simple line-oriented text editor\n\n"
"Usage: edit <path>\n"
"Opens a minimal line editor for the file in the terminal. For a\n"
"full-screen editor with selection/find-replace, use the GUI\n"
"Notepad app instead.\n\n"
"Example:\n"
"  /> edit /notes/todo.txt\n"
},
{"tsharp",
"tsharp - run the T# 4.1 Lite interpreter\n\n"
"Usage: tsharp [path]\n"
"With a path, runs that .ts script and exits. With no argument,\n"
"starts an interactive T# REPL. T# is tOS's own Turkish-keyword\n"
"scripting language: yazdir (print), degisken (variable), eger\n"
"(if), dongu (loop), fonksiyon (function). It also has built-in\n"
"system functions — see 'man tsharp_api'.\n\n"
"Examples:\n"
"  /> tsharp\n"
"  >> yazdir(\"merhaba\")\n"
"  /> tsharp /notes/script.ts\n"
},
{"python",
"python - run the embedded MicroPython interpreter\n\n"
"Usage: python [path]\n"
"With a path, compiles and runs that .py file directly out of\n"
"tOS's filesystem (no real OS-level file handles are involved —\n"
"the whole file is read into memory and executed). With no\n"
"argument, starts an interactive REPL. The 'tos' module exposes\n"
"the same system API as T#'s builtins (see 'man tos_api'), and the\n"
"'tosgui' module lets a script open its own GUI window (see\n"
"'man tosgui').\n\n"
"Examples:\n"
"  /> python\n"
"  >>> import tos\n"
"  >>> tos.uptime()\n"
"  /> python /programs/tosgui_demo.py\n"
},
{"reboot",
"reboot - restart the machine\n\n"
"Triggers an immediate reboot via the keyboard controller. Nothing\n"
"is saved first, so flush any pending writes before running this.\n\n"
"Example:\n"
"  /> reboot\n"
},
{"shutdown",
"shutdown - power off the machine\n\n"
"Halts the system. On real hardware/most VMs this powers the\n"
"machine off; nothing is saved first.\n\n"
"Example:\n"
"  /> shutdown\n"
},
{"version",
"version - print the tOS version string\n\n"
"Example:\n"
"  /> version\n"
"  tOS v0.9.xx\n"
},
{"about",
"about - show the About window/info screen\n\n"
"Prints (or, in GUI mode, opens a window with) basic information\n"
"about this tOS build.\n\n"
"Example:\n"
"  /> about\n"
},
{"uname",
"uname - print system identification\n\n"
"Prints a short line identifying the OS/kernel, similar in spirit\n"
"to Unix's uname.\n\n"
"Example:\n"
"  /> uname\n"
},
{"whoami",
"whoami - print the name of the user the shell runs as\n\n"
"After login this is your account; inside 'su' or 'sudo' it is the\n"
"account the commands run as (usually root).\n\n"
"Example:\n"
"  /> whoami\n"
},
{"hostname",
"hostname - print (or, as root, set) the computer name\n\n"
"Usage: hostname [name]\n"
"The name is stored in /etc/hostname and shows in the prompt.\n\n"
"Example:\n"
"  $ hostname\n"
"  # hostname mybox\n"
},
{"date",
"date - print the current date and time\n\n"
"Reads the time from the CMOS real-time clock.\n\n"
"Example:\n"
"  /> date\n"
},
{"cal",
"cal - print a calendar\n\n"
"Shows a simple calendar for the current month.\n\n"
"Example:\n"
"  /> cal\n"
},
{"yes",
"yes - print a string repeatedly\n\n"
"Usage: yes [string]\n"
"Prints 'string' (or 'y' if omitted) forever, one line at a time.\n"
"Mostly useful for testing or piping into something that reads a\n"
"fixed number of lines.\n\n"
"Example:\n"
"  /> yes hello\n"
},
{"seq",
"seq - print a sequence of numbers\n\n"
"Usage: seq <last> | seq <first> <last>\n"
"Prints integers from 'first' (default 1) to 'last' inclusive, one\n"
"per line.\n\n"
"Examples:\n"
"  /> seq 5\n"
"  /> seq 3 7\n"
},
{"sleep",
"sleep - pause for a number of seconds\n\n"
"Usage: sleep <seconds>\n"
"Blocks the shell for the given duration before returning to the\n"
"prompt.\n\n"
"Example:\n"
"  /> sleep 2\n"
},
{"df",
"df - show filesystem disk usage\n\n"
"Lists mounted filesystems with their used/free space.\n\n"
"Example:\n"
"  /> df\n"
},
{"free",
"free - show memory usage\n\n"
"Prints used/free RAM, in the same spirit as Unix's 'free'. Useful\n"
"for spotting leaks while testing new code.\n\n"
"Example:\n"
"  /> free\n"
},
{"dmesg",
"dmesg - print kernel log messages\n\n"
"Dumps the in-memory kernel log ring buffer (boot messages,\n"
"driver init, [OK]/[WARN]/[INFO] lines).\n\n"
"Example:\n"
"  /> dmesg\n"
},
{"basename",
"basename - strip the directory part of a path\n\n"
"Usage: basename <path>\n"
"Example:\n"
"  /> basename /notes/todo.txt\n"
"  todo.txt\n"
},
{"dirname",
"dirname - strip the filename part of a path\n\n"
"Usage: dirname <path>\n"
"Example:\n"
"  /> dirname /notes/todo.txt\n"
"  /notes\n"
},
{"which",
"which - show which builtin a command name resolves to\n\n"
"Usage: which <command>\n"
"All commands here are shell builtins, so this mostly confirms a\n"
"name is recognized rather than pointing at a binary on disk.\n\n"
"Example:\n"
"  /> which ls\n"
},
{"env",
"env - print the current shell environment\n\n"
"Lists environment-style variables the shell knows about (aliases,\n"
"current directory, etc., depending on build).\n\n"
"Example:\n"
"  /> env\n"
},
{"notify",
"notify - show a notification on the desktop\n\n"
"Usage: notify [-a app] [-i icon] [-b button]... [-w] <title> [body]\n"
"       notify dnd on|off\n\n"
"  -a app     the app it comes from (its icon and name are shown)\n"
"  -i icon    an icon: an app id (paint, files) or an image (doc48_disk)\n"
"  -b label   a button; give it twice for two\n"
"  -w         wait for the answer and print it\n\n"
"Example:\n"
"  /> notify -b Install -b Later -w \"Update ready\" \"tOS 2.1 can be installed now.\"\n"
},
{"uptime",
"uptime - show how long the system has been running\n\n"
"Example:\n"
"  /> uptime\n"
},
{"ps",
"ps - list running processes/tasks\n\n"
"Shows each task's PID and state from the scheduler. See 'htop' for\n"
"a live-updating view, and 'kill' to terminate one.\n\n"
"Example:\n"
"  /> ps\n"
},
{"log",
"log - running tasks plus the full kernel/operation log, in one place\n\n"
"Prints two sections: the same task table 'ps' shows, followed by the\n"
"entire in-memory kernel log ('dmesg' shows the same log on its own)\n"
"— boot messages, driver init lines, and detailed disk-operation\n"
"entries logged by mount/unmount/format (kernel/fs/diskops.c): each\n"
"operation logs a start line, a result line, and — for format —\n"
"a hex dump of the first 128 bytes of sector 0 read back from disk\n"
"afterward, so you can see real evidence of what landed on the disk\n"
"instead of just trusting a return code. Any mount/format/unmount\n"
"failure (from the 'disk' command or the Disk Utility GUI app) is\n"
"logged here too, with the same message shown to the user.\n\n"
"Example:\n"
"  /> disk format ata0 fat32\n"
"  /> log\n"
"  === Running tasks ===\n"
"   PID  NAME         STATE\n"
"  ...\n"
"  === Kernel / operation log (boot, disk ops, hex dumps) ===\n"
"  ...\n"
"  diskops: formatting ata0 as...\n"
"  fat32\n"
"  diskops: formatted ata0 OK\n"
"  diskops: sector 0 after format:\n"
"  0000  EB 58 90 4D 53 44 4F 53  35 2E 30 00 02 08 20 00  |.X.MSDOS5.0... |\n"
"  ...\n\n"
"See also: 'man dmesg' (just the log, no task table), 'man disk'.\n"
},
{"htop",
"htop - live process monitor\n\n"
"A continuously-refreshing view of running tasks, similar to Linux's\n"
"htop. Press a key (commonly q) to exit back to the shell.\n\n"
"Example:\n"
"  /> htop\n"
},
{"disk",
"disk - manage storage disks\n\n"
"Usage: disk list | disk info <dev> | disk mount <dev> <path> |\n"
"       disk umount <path> | disk format <dev> <fs>\n"
"Lists detected block devices and lets you mount/unmount/format\n"
"them. 'disk list' first to see device names. Every mount/umount/\n"
"format here (and from the Disk Utility GUI app) is recorded in the\n"
"kernel log with a hex dump of sector 0 after a format — see 'man\n"
"log' to read it back.\n\n"
"Examples:\n"
"  /> disk list\n"
"  /> disk mount ata0 /mnt\n"
},
{"tar",
"tar - create, extract, or list a ustar (POSIX tar) archive\n\n"
"Usage: tar c <archive.tar> <file...>   create (files/dirs added\n"
"                                        recursively)\n"
"       tar x <archive.tar> [dest_dir]  extract (default: cwd)\n"
"       tar t <archive.tar>             list entries and sizes\n\n"
"From-scratch ustar reader/writer — no compression, same as real\n"
"tar without a -z/-j flag. Directories are stored and recreated\n"
"automatically, including nested ones.\n\n"
"Examples:\n"
"  /> tar c backup.tar /notes /etc/config.txt\n"
"  /> tar t backup.tar\n"
"  /> mkdir /restore\n"
"  /> tar x backup.tar /restore\n\n"
"See also: 'man zip' for a compressed-friendlier, more universally\n"
"recognized archive format.\n"
},
{"zip",
"zip - create a zip archive\n\n"
"Usage: zip <archive.zip> <file...>\n"
"Files/directories are added recursively. Entries are stored\n"
"uncompressed (method 0) — tOS has a DEFLATE *decoder* (built for\n"
"reading PNG files) but no encoder, so this writes a perfectly valid,\n"
"ordinary zip file any unzip tool can open, just without the size\n"
"savings real compression would give.\n\n"
"Example:\n"
"  /> zip notes.zip /notes\n\n"
"See also: 'man unzip', 'man tar'.\n"
},
{"unzip",
"unzip - extract a zip archive\n\n"
"Usage: unzip <archive.zip> [dest_dir]\n"
"Extracts every entry under dest_dir (default: cwd), recreating\n"
"directories as needed. Understands both stored (method 0) and\n"
"deflated (method 8) entries, so — unlike 'zip', which only writes\n"
"stored archives — this can open real-world zip files made by other\n"
"tools, not just ones tOS made itself.\n\n"
"Example:\n"
"  /> mkdir /restore\n"
"  /> unzip notes.zip /restore\n\n"
"See also: 'man zip', 'man tar'.\n"
},
{"kill",
"kill - terminate a running task\n\n"
"Usage: kill <pid>\n"
"Use 'ps' first to find the PID of the task to terminate.\n\n"
"Example:\n"
"  /> ps\n"
"  /> kill 4\n"
},
{"chmod",
"chmod - change a file's permission bits\n\n"
"Usage: chmod <mode> <path>...\n"
"'mode' is octal (644, 755, 1777) or symbolic (u+x, go-w, a=r). Only the\n"
"owner of a file, or root, may change it.\n\n"
"Example:\n"
"  $ chmod 600 secret.txt\n"
"  $ chmod g+w notes.txt\n"
},
{"chown",
"chown - change who owns a file\n\n"
"Usage: chown <user>[:<group>] <path>...\n"
"Only root may give files away; an owner may only change the group, to\n"
"one of his own groups.\n\n"
"Example:\n"
"  # chown alice:users /home/alice/report.txt\n"
},
{"chgrp",
"chgrp - change the group of a file\n\n"
"Usage: chgrp <group> <path>...\n\n"
"Example:\n"
"  $ chgrp users report.txt\n"
},
{"umask",
"umask - show or set the permissions removed from new files\n\n"
"Usage: umask [mask]\n"
"The default 022 makes new files 644 and new directories 755.\n\n"
"Example:\n"
"  $ umask 077\n"
},
{"id",
"id - print the user and group ids of a user\n\n"
"Usage: id [user]\n\n"
"Example:\n"
"  $ id\n"
"  uid=1000(alice) gid=1000(alice) groups=1000(alice),27(sudo)\n"
},
{"groups",
"groups - print the groups a user belongs to\n\n"
"Usage: groups [user]\n"
},
{"passwd",
"passwd - change a password\n\n"
"Usage: passwd [user]\n"
"You are asked for the current password (root is not), then the new one\n"
"twice. Passwords are stored salted and hashed in /etc/shadow, which only\n"
"root can read.\n"
},
{"useradd",
"useradd - create a user account (root)\n\n"
"Usage: useradd [-G sudo] <name>\n"
"Asks for the new password, makes /home/<name> and a private group.\n"
"With -G sudo the user may use sudo.\n\n"
"Example:\n"
"  # useradd -G sudo bob\n"
},
{"userdel",
"userdel - remove a user account (root)\n\n"
"Usage: userdel <name>\n"
"The home directory and its files are kept.\n"
},
{"su",
"su - start a shell as another user\n\n"
"Usage: su [-] [user]\n"
"Without a name you become root. You must give that user's password\n"
"unless you are root already. 'exit' returns to your own shell.\n"
},
{"sudo",
"sudo - run one command as root (or another user)\n\n"
"Usage: sudo [-u user] <command> [args...]\n"
"       sudo -i        a root shell\n"
"       sudo -k        forget the password for now\n"
"Allowed for root and for members of the 'sudo' group; you give your own\n"
"password, which is remembered for five minutes. The password also\n"
"counts as the confirmation that deleting files under /system needs.\n\n"
"Example:\n"
"  $ sudo tpkg install git\n"
},
{"stat",
"stat - show a file's size, permissions and owner\n\n"
"Usage: stat <path>\n"
},
{"exit",
"exit - leave the current shell (also: logout)\n\n"
"In your login shell this returns to the login prompt; inside su or sudo -i\n"
"it returns to the shell you came from.\n"
},
{"hexdump",
"hexdump - show a file's raw bytes in hex\n\n"
"Usage: hexdump <path>\n"
"Useful for inspecting binary files or debugging file-format code.\n\n"
"Example:\n"
"  /> hexdump /assets/doom1.wad\n"
},
{"tee",
"tee - write input to a file while also printing it\n\n"
"Usage: tee <path>\n"
"Reads lines typed at the terminal and writes each one both to the\n"
"screen and to 'path', until an empty line/EOF ends it.\n\n"
"Example:\n"
"  /> tee /notes/todo.txt\n"
},
{"alias",
"alias - define or list command aliases\n\n"
"Usage: alias [name=command]\n"
"With no argument, lists all current aliases. With 'name=command',\n"
"defines a new one; typing 'name' afterwards runs 'command'.\n\n"
"Examples:\n"
"  /> alias\n"
"  /> alias ll=\"ls -l\"\n"
},
{"unalias",
"unalias - remove a command alias\n\n"
"Usage: unalias <name>\n"
"Example:\n"
"  /> unalias ll\n"
},
{"history",
"history - show previously run commands\n\n"
"Prints the shell's command history list, most recent commands\n"
"included. Use the up/down arrows at the prompt to step through it\n"
"live.\n\n"
"Example:\n"
"  /> history\n"
},
{"font",
"font - list or change the terminal font style\n\n"
"Usage: font            (list available styles)\n"
"       font <name|n>   (switch to that style)\n\n"
"Examples:\n"
"  /> font\n"
"  /> font 1\n"
},
{"tos_api",
"tos_api - the system API shared by MicroPython and T#\n\n"
"Both scripting languages embedded in tOS (MicroPython's 'tos'\n"
"module, and T#'s built-in functions) are thin wrappers around the\n"
"same underlying C API, so they can do the same things: run shell\n"
"commands, read/write files, manage processes, and open apps.\n\n"
"MicroPython name      T# built-in        What it does\n"
"  tos.exec(cmd)          calistir(cmd)       run a shell command,\n"
"                                              return its output\n"
"  tos.read(path)         dosyaoku(yol)       read a whole file\n"
"  tos.write(path,data)   dosyayaz(yol,veri)  write/overwrite a file\n"
"  tos.mkdir(path)        klasoryap(yol)      create a directory\n"
"  tos.delete(path)       dosyasil(yol)       delete a file\n"
"  tos.exists(path)       dosyavarmi(yol)     check if a path exists\n"
"  tos.list(path)         listele(yol)        list a directory\n"
"  tos.open_app(name)     uygulamaac(isim)    open a GUI app by name\n"
"  tos.ps()               surecler()          list running tasks\n"
"  tos.kill(pid)          sureldur(pid)       kill a task by PID\n"
"  tos.uptime()           calismasuresi()     seconds since boot\n"
"  tos.http_get(url)      agetir(url)         fetch a URL's body over\n"
"                                              plain HTTP (no https://)\n\n"
"Examples (MicroPython):\n"
"  >>> import tos\n"
"  >>> tos.write(\"/notes/hi.txt\", \"hello\\n\")\n"
"  >>> print(tos.read(\"/notes/hi.txt\"))\n"
"  >>> print(tos.exec(\"ls /notes\"))\n"
"  >>> tos.open_app(\"calculator\")\n"
"  >>> print(tos.http_get(\"http://example.com/\"))\n\n"
"Examples (T#):\n"
"  >> dosyayaz(\"/notes/hi.txt\", \"merhaba\\n\")\n"
"  >> yazdir(dosyaoku(\"/notes/hi.txt\"))\n"
"  >> yazdir(calistir(\"ls /notes\"))\n"
"  >> uygulamaac(\"calculator\")\n"
"  >> yazdir(agetir(\"http://example.com/\"))\n\n"
"See also: 'man tsharp_api' (T# details), 'man tosgui' (GUI module).\n"
},
{"tsharp_api",
"tsharp_api - T# built-in functions in detail\n\n"
"T# is run via the 'tsharp' command (see 'man tsharp'). Besides the\n"
"language keywords (yazdir/degisken/eger/dongu/fonksiyon), it has\n"
"these built-in functions, which all call into the same system API\n"
"as MicroPython's 'tos' module (see 'man tos_api' for the mapping):\n\n"
"  calistir(komut)        - run a shell command, return its output\n"
"  dosyaoku(yol)           - read a whole file as a string\n"
"  dosyayaz(yol, veri)     - write/overwrite a file with 'veri'\n"
"  klasoryap(yol)          - create a directory\n"
"  dosyasil(yol)           - delete a file\n"
"  dosyavarmi(yol)         - 1 if the path exists, 0 otherwise\n"
"  listele(yol)            - list a directory's entries\n"
"  uygulamaac(isim)        - open a GUI app by name (e.g. \"notepad\")\n"
"  surecler()              - list running tasks\n"
"  sureldur(pid)           - kill the task with that PID\n"
"  calismasuresi()         - seconds since boot, as a number\n"
"  agetir(url)             - fetch a URL's body over plain HTTP only\n"
"                             (no https://); truncated to 256 bytes\n\n"
"Example script:\n"
"  degisken ad = \"tOS\"\n"
"  yazdir(\"merhaba \" + ad)\n"
"  eger dosyavarmi(\"/notes/todo.txt\") ise\n"
"      yazdir(dosyaoku(\"/notes/todo.txt\"))\n"
"  degilse\n"
"      yazdir(\"dosya yok\")\n"
"  son\n"
},
{"tosgui",
"tosgui - tkinter-like GUI module for MicroPython scripts\n\n"
"tOS has no pixel graphics, so 'widgets' are text drawn at a\n"
"row/column with a color; your script does its own hit-testing\n"
"against the coordinates it drew at. Import it from a .py file run\n"
"with 'python <path>' (see 'man python') — it's meant for scripts\n"
"that open a window, run an event loop, and close it again, not for\n"
"one-off REPL lines.\n\n"
"  tosgui.open(title)      - open a window; False if one is already\n"
"                            open (only one tosgui window at a time)\n"
"  tosgui.close()          - close the window\n"
"  tosgui.clear()          - clear the window's contents\n"
"  tosgui.text(x,y,s,fg=,bg=)   - draw a string at (x,y)\n"
"  tosgui.button(x,y,s,fg=,bg=) - draw a '[ s ]' style button\n"
"  tosgui.poll_click()     - (x,y) tuple if clicked since last poll,\n"
"                            else None\n"
"  tosgui.poll_key()       - key code if a key was pressed, else\n"
"                            None (only while the window has focus)\n"
"  tosgui.has_focus()      - True if this window is focused\n"
"  tosgui.update()         - pump the event loop; call this once per\n"
"                            loop iteration\n"
"  tosgui.input(x,y,prompt=\"\") - draw prompt, block until Enter, and\n"
"                            return the typed string (backspace works);\n"
"                            for asking a single question, not for use\n"
"                            inside an open event loop\n"
"  Color constants: BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN,\n"
"  LIGHT_GREY, DARK_GREY, LIGHT_BLUE, LIGHT_GREEN, LIGHT_CYAN,\n"
"  LIGHT_RED, LIGHT_MAGENTA, YELLOW, WHITE\n\n"
"Example (click counter, also shipped as\n"
"/programs/tosgui_demo.py — run it with 'python\n"
"/programs/tosgui_demo.py'):\n"
"  import tosgui\n"
"  count = 0\n"
"  tosgui.open(\"demo\")\n"
"  tosgui.text(2, 2, \"clicks: 0\")\n"
"  tosgui.button(2, 4, \"click me\")\n"
"  running = True\n"
"  while running:\n"
"      click = tosgui.poll_click()\n"
"      if click:\n"
"          x, y = click\n"
"          if y == 4:\n"
"              count += 1\n"
"              tosgui.text(2, 2, \"clicks: \" + str(count))\n"
"      key = tosgui.poll_key()\n"
"      if key is not None and chr(key) == 'q':\n"
"          running = False\n"
"      tosgui.update()\n"
"  tosgui.close()\n\n"
"Example (tosgui.input):\n"
"  tosgui.open(\"asker\")\n"
"  name = tosgui.input(2, 2, \"Adin nedir? \")\n"
"  tosgui.text(2, 4, \"merhaba \" + name)\n"
"  tosgui.update()\n"
},
{0, 0}
};

static void print_man_list(void)
{
    terminal_writestring("Available man pages:\n\n");
    terminal_writestring("Commands:\n  ");
    int col = 0;
    for (int i = 0; man_pages[i].name; i++) {
        if (strcmp(man_pages[i].name, "tos_api") == 0) break;
        terminal_writestring(man_pages[i].name);
        terminal_writestring("  ");
        col++;
        if (col % 8 == 0) terminal_writestring("\n  ");
    }
    terminal_writestring("\n\nScripting topics:\n");
    terminal_writestring("  tos_api      - C API shared by MicroPython and T#\n");
    terminal_writestring("  tsharp_api   - T# built-in functions in detail\n");
    terminal_writestring("  tosgui       - MicroPython GUI module\n\n");
    terminal_writestring("Run 'man <name>' for the full page.\n");
}

void cmd_man(int argc, char **args)
{
    if (argc < 2) {
        print_man_list();
        return;
    }
    for (int i = 0; man_pages[i].name; i++) {
        if (strcmp(args[1], man_pages[i].name) == 0) {
            terminal_writestring(man_pages[i].body);
            return;
        }
    }
    terminal_writestring("man: no manual entry for '");
    terminal_writestring(args[1]);
    terminal_writestring("'\nRun 'man' with no arguments to see the list.\n");
}
