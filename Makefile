CC = gcc
CXX = g++
AS = as
LD = ld
AR = ar

GCC_INC := $(shell $(CC) -m32 -print-file-name=include)

# Regular kernel flags (no nostdinc to avoid breaking existing code)
CFLAGS = -MMD -MP -m32 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
         -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
         -mno-mmx -mno-sse -mno-sse2 \
         -O2 -Wall -Wextra -Werror \
         -I. \
         -Ikernel/core -Ikernel/display \
         -Ikernel/fs -Ikernel/shell -Ikernel/shell/commands -Ikernel/lib -Ikernel/net \
          -Ikernel/drivers/include -Ikernel/drivers/bus -Ikernel/drivers/storage \
          -Ikernel/drivers/net -Ikernel/drivers/usb -Ikernel/drivers/audio \
          -Ikernel/drivers/video -Ikernel/drivers/input -Ikernel/drivers/system \
          -Ikernel/drivers/misc -Ikernel/micropython \
         -Ikernel/micropython/ports/tos \
         -Ikernel/audio

# Relaxed flags for MicroPython core (uses -isystem for 32-bit glibc compat)
MPY_CFLAGS = -MMD -MP -m32 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
             -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
             -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
             -mno-mmx -mno-sse -mno-sse2 \
             -O2 -Wall -Wno-unused-variable -Wno-unused-function \
             -Wno-unused-parameter -Wno-sign-compare \
             -Wno-missing-field-initializers -Wno-implicit-fallthrough \
             -isystem /tmp/i386-linux-gnu \
             -I. \
             -Ikernel/micropython \
             -Ikernel/micropython/ports/tos \
             -Ikernel/micropython/genhdr

# Port files need kernel headers too
MPY_PORT_CFLAGS = $(MPY_CFLAGS) \
             -Ikernel/core -Ikernel/display \
             -Ikernel/fs -Ikernel/shell -Ikernel/lib -Ikernel/net \
              -Ikernel/drivers/include -Ikernel/drivers/bus -Ikernel/drivers/storage \
              -Ikernel/drivers/net -Ikernel/drivers/usb -Ikernel/drivers/audio \
              -Ikernel/drivers/video -Ikernel/drivers/input -Ikernel/drivers/system \
              -Ikernel/drivers/misc

# 1990s vendored C (id Software/doomgeneric) trips plenty of things
# -Wall/-Wextra/-Werror rightly reject in tOS's own code, same reason
# MicroPython gets its own relaxed MPY_CFLAGS above. -Ikernel/doom/port
# comes first so <string.h>/<stdlib.h>/<math.h>/etc resolve to that
# directory's wrapper headers (which #include_next through to the real
# kernel/lib ones, adding only the handful of extra declarations DOOM
# needs -- see kernel/doom/port/doom_compat.c) instead of kernel/lib's
# directly.
DOOM_CFLAGS = -MMD -MP -m32 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
             -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
             -mno-mmx -mno-sse -mno-sse2 \
             -O2 -Wall -Wno-unused-variable -Wno-unused-function \
             -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable \
             -Wno-missing-field-initializers -Wno-implicit-fallthrough \
             -Wno-missing-braces -Wno-parentheses -Wno-format \
             -Wno-type-limits -Wno-unused-value \
             -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DFEATURE_SOUND \
             -Ikernel/doom/port -Ikernel/doom \
             -I. -Ikernel/core -Ikernel/display \
             -Ikernel/fs -Ikernel/shell -Ikernel/shell/commands -Ikernel/lib -Ikernel/net \
             -Ikernel/drivers/include -Ikernel/drivers/bus -Ikernel/drivers/storage \
             -Ikernel/drivers/net -Ikernel/drivers/usb -Ikernel/drivers/audio \
             -Ikernel/drivers/video -Ikernel/drivers/input -Ikernel/drivers/system \
             -Ikernel/drivers/misc -Ikernel/audio

LDFLAGS = -m elf_i386 -T kernel/boot/linker.ld
ASFLAGS = --32

# Vendored C++ (Wolf4SDL, itself a port of id Software's original
# Wolfenstein 3D) -- unlike doomgeneric, this engine calls SDL2
# directly from within its own source rather than through a small
# platform-specific shim, so kernel/wolf3d/port/ provides SDL.h/
# SDL_mixer.h/SDL_syswm.h compatibility headers backed by tOS's own
# framebuffer/keyboard/mouse/audio code instead of a real SDL2. This
# is a from-scratch freestanding C++ setup (no libstdc++, no
# exceptions/RTTI/threads -- tOS has no thread-safe statics or stack
# unwinding support), same general idea as DOOM_CFLAGS below but with
# g++-specific flags added.
WOLF_CXXFLAGS = -MMD -MP -m32 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
             -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
             -fno-threadsafe-statics -fno-stack-protector -fno-pic -fno-pie \
             -mno-mmx -mno-sse -mno-sse2 \
             -O2 -Wall -Wno-unused-variable -Wno-unused-function \
             -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable \
             -Wno-missing-field-initializers -Wno-implicit-fallthrough \
             -Wno-missing-braces -Wno-parentheses -Wno-format \
             -Wno-type-limits -Wno-unused-value -Wno-reorder \
             -Ikernel/wolf3d/port -Ikernel/wolf3d \
             -I. -Ikernel/core -Ikernel/display \
             -Ikernel/fs -Ikernel/shell -Ikernel/shell/commands -Ikernel/lib -Ikernel/net \
             -Ikernel/drivers/include -Ikernel/drivers/bus -Ikernel/drivers/storage \
             -Ikernel/drivers/net -Ikernel/drivers/usb -Ikernel/drivers/audio \
             -Ikernel/drivers/video -Ikernel/drivers/input -Ikernel/drivers/system \
             -Ikernel/drivers/misc -Ikernel/audio

WOLF_CFLAGS = -MMD -MP -m32 -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
             -fno-stack-protector -fno-pic -fno-pie -fno-builtin \
             -mno-mmx -mno-sse -mno-sse2 \
             -O2 -Wall -Wno-unused-variable -Wno-unused-function \
             -Wno-unused-parameter -Wno-sign-compare -Wno-unused-but-set-variable \
             -Wno-missing-field-initializers -Wno-implicit-fallthrough \
             -Wno-missing-braces -Wno-parentheses -Wno-format \
             -Wno-type-limits -Wno-unused-value \
             -Ikernel/wolf3d/port -Ikernel/wolf3d \
             -I. -Ikernel/core -Ikernel/display \
             -Ikernel/fs -Ikernel/shell -Ikernel/shell/commands -Ikernel/lib -Ikernel/net \
             -Ikernel/drivers/include -Ikernel/drivers/bus -Ikernel/drivers/storage \
             -Ikernel/drivers/net -Ikernel/drivers/usb -Ikernel/drivers/audio \
             -Ikernel/drivers/video -Ikernel/drivers/input -Ikernel/drivers/system \
             -Ikernel/drivers/misc -Ikernel/audio

# Auto-discover driver sources
DRIVER_SRCS := $(wildcard kernel/drivers/bus/*.c kernel/drivers/storage/*.c \
    kernel/drivers/net/*.c kernel/drivers/usb/*.c kernel/drivers/audio/*.c \
    kernel/drivers/video/*.c kernel/drivers/input/*.c kernel/drivers/system/*.c \
    kernel/drivers/misc/*.c)
DRIVER_OBJS := $(DRIVER_SRCS:.c=.o)

KERNEL_OBJS = \
    kernel/boot/boot.o \
    kernel/boot/isr_stubs.o \
    kernel/core/kernel.o \
    kernel/display/terminal.o \
    kernel/lib/string.o \
    kernel/core/gdt.o \
    kernel/core/idt.o \
    kernel/core/isr.o \
    kernel/core/irq.o \
    kernel/lib/memory.o \
    kernel/lib/errno.o \
    kernel/lib/stdio.o \
    kernel/lib/stdlib.o \
    kernel/lib/ctype.o \
    kernel/lib/udivdi3.o \
    kernel/fs/fs.o \
    kernel/fs/vfs.o \
    kernel/fs/fsbridge.o \
    kernel/fs/syscall.o \
    kernel/core/serial.o \
    kernel/core/debugmon.o \
    kernel/core/sound.o \
    kernel/core/scheduler.o \
    kernel/core/auth.o \
    kernel/core/paging.o \
    kernel/core/tss.o \
    kernel/core/usermode.o \
    kernel/core/proc.o \
    kernel/shell/shell.o \
    kernel/shell/commands/cmd_file.o \
    kernel/shell/commands/cmd_fs.o \
    kernel/shell/commands/cmd_sys.o \
    kernel/shell/commands/cmd_man.o \
    kernel/shell/commands/cmd_util.o \
    kernel/shell/commands/cmd_net.o \
    kernel/shell/commands/cmd_htop.o \
    kernel/shell/commands/cmd_disk.o \
    kernel/shell/commands/cmd_archive.o \
    kernel/shell/commands/cmd_user.o \
    kernel/shell/commands/cmd_tpkg.o \
    kernel/core/klog.o \
    kernel/fs/ramfs.o \
    kernel/fs/perm.o \
    kernel/fs/sysdirs.o \
    kernel/fs/tfsk.o \
    kernel/fs/fat16.o \
    kernel/fs/fat32.o \
    kernel/fs/exfat.o \
    kernel/fs/ext2.o \
    kernel/fs/ext3.o \
    kernel/fs/ext4.o \
    kernel/fs/ntfs.o \
    kernel/fs/btrfs.o \
    kernel/fs/xfs.o \
    kernel/fs/zfs.o \
    kernel/fs/apfs.o \
    kernel/fs/diskops.o \
    kernel/fs/tarfmt.o \
    kernel/fs/zipfmt.o \
    kernel/fs/installer.o \
    kernel/shell/tsharp.o \
    kernel/shell/tos_api.o \
    kernel/display/gui.o \
    kernel/display/wm.o \
    kernel/display/wm_gfx.o \
    kernel/display/gfx.o \
    kernel/display/ui_assets.o \
    kernel/display/uiapp.o \
    kernel/display/uihost_tos.o \
    kernel/display/apps/calc.o \
    kernel/display/apps/clock.o \
    kernel/display/apps/g2048.o \
    kernel/display/apps/notepad2.o \
    kernel/display/apps/files.o \
    kernel/display/apps/viewer2.o \
    kernel/display/apps/activity.o \
    kernel/display/apps/network.o \
    kernel/display/apps/paint2.o \
    kernel/display/apps/notes.o \
    kernel/display/apps/settings.o \
    kernel/display/imgload.o \
    kernel/display/jpeg.o \
    kernel/display/notepad.o \
    kernel/display/clock.o \
    kernel/display/about.o \
    kernel/display/diskmgr.o \
    kernel/display/calculator.o \
    kernel/display/filemgr.o \
    kernel/display/paint.o \
    kernel/display/png.o \
    kernel/display/viewer.o \
    kernel/display/taskmgr.o \
    kernel/display/fbconsole.o \
    kernel/net/net.o \
    kernel/net/nic.o \
    kernel/net/rtl8139.o \
    kernel/net/pcnet.o \
    kernel/net/e1000.o \
    kernel/net/virtio_net.o \
    kernel/net/ne2000.o \
    kernel/net/arp.o \
    kernel/net/ip.o \
    kernel/net/icmp.o \
    kernel/net/udp.o \
    kernel/net/dns.o \
    kernel/net/dhcp.o \
    kernel/net/tcp.o \
    kernel/net/sctp.o \
    kernel/net/dccp.o \
    kernel/net/udplite.o \
    kernel/net/ip6.o \
    kernel/net/icmpv6.o \
    kernel/net/ipsec.o \
    kernel/net/vlan.o \
    kernel/net/bridge.o \
    kernel/net/bonding.o \
    kernel/net/ipx.o \
    kernel/net/route.o \
    kernel/net/fw.o \
    kernel/net/gre.o \
    kernel/net/ipip.o \
    kernel/net/chacha20.o \
    kernel/net/wgtun.o \
    kernel/net/sha256.o \
    kernel/net/sha1.o \
    kernel/net/websocket.o \
    kernel/net/sha512.o \
    kernel/net/rsa.o \
    kernel/net/x509.o \
    kernel/net/ca_store.o \
    kernel/net/csprng.o \
    kernel/net/aes.o \
    kernel/net/gcm.o \
    kernel/net/x25519.o \
    kernel/net/p256.o \
    kernel/net/bignum.o \
    kernel/net/tls.o \
    kernel/net/https.o \
    kernel/net/http.o \
    kernel/audio/audio.o \
    kernel/audio/wav_decoder.o \
    kernel/audio/mp3_decoder.o \
    kernel/audio/aac_decoder.o \
    kernel/audio/m4a_demux.o \
        kernel/audio/demo_song_embed.o \
    kernel/display/mediaplayer.o \
    kernel/display/netmon.o \
    kernel/display/snake.o \
    kernel/display/game2048.o \
    kernel/display/pdf_parse.o \
    kernel/display/pdfview.o \
    kernel/display/stickynotes.o \
    kernel/micropython/ports/tos/tos_main.o \
    kernel/micropython/ports/tos/tos_hal.o \
    kernel/micropython/ports/tos/math_stubs.o \
    kernel/micropython/ports/tos/tos_stubs.o \
    kernel/micropython/ports/tos/modtos.o \
    kernel/micropython/ports/tos/modtosgui.o \
    kernel/micropython/ports/tos/modos.o \
    kernel/micropython/ports/tos/modtime.o \
    kernel/micropython/ports/tos/modjson.o \
    kernel/micropython/ports/tos/modsocket.o \
    kernel/micropython/ports/tos/modgitcrypto.o \
    kernel/micropython/ports/tos/runfile.o \
    kernel/micropython/shared/runtime/pyexec.o \
    kernel/micropython/shared/readline/readline.o

KERNEL_OBJS += $(DRIVER_OBJS)

# MicroPython py/ core sources (exclude native/asm/persistent)
MPY_PY_SRCS := $(filter-out %/asmarm.c %/asmthumb.c %/asmxtensa.c %/asmrv32.c \
    %/emitnative.c %/emitinlinethumb.c %/emitinlinextensa.c %/emitinlinerrv32.c \
    %/persistentcode.c, $(wildcard kernel/micropython/py/*.c))
MPY_PY_OBJS := $(MPY_PY_SRCS:.c=.o)
KERNEL_OBJS += $(MPY_PY_OBJS)

# Vendored id Software/doomgeneric source (kernel/doom/LICENSE, GPLv2)
# plus tOS's own platform glue (kernel/doom/port/) -- see README's
# "Linear framebuffer graphics" section.
DOOM_SRCS := $(wildcard kernel/doom/*.c) $(wildcard kernel/doom/port/*.c)
DOOM_OBJS := $(DOOM_SRCS:.c=.o)
KERNEL_OBJS += $(DOOM_OBJS)

# Vendored Wolf4SDL source (kernel/wolf3d/LICENSE, GPLv2) -- itself a
# port of id Software's original Wolfenstein 3D to SDL2 -- plus tOS's
# own SDL2 compatibility shim (kernel/wolf3d/port/).
WOLF_CXX_SRCS := $(wildcard kernel/wolf3d/*.cpp) $(wildcard kernel/wolf3d/port/*.cpp)
WOLF_C_SRCS := $(wildcard kernel/wolf3d/*.c) $(wildcard kernel/wolf3d/port/*.c)
WOLF_ASM_SRCS := $(wildcard kernel/wolf3d/port/*.s)
WOLF_OBJS := $(WOLF_CXX_SRCS:.cpp=.o) $(WOLF_C_SRCS:.c=.o) $(WOLF_ASM_SRCS:.s=.o)
KERNEL_OBJS += $(WOLF_OBJS)

PROGRAMS = programs/tosgui_demo.py programs/hello.t programs/bsstest.t \
	programs/filetest.t programs/gfxtest.t programs/termtest.t programs/nettest.t programs/cryptotest.t assets/doom1.wad \
	assets/wolf3d/audiohed.wl1 assets/wolf3d/audiot.wl1 assets/wolf3d/gamemaps.wl1 \
	assets/wolf3d/maphead.wl1 assets/wolf3d/vgadict.wl1 assets/wolf3d/vgagraph.wl1 \
	assets/wolf3d/vgahead.wl1 assets/wolf3d/vswap.wl1

.PHONY: all clean run iso

all: tOS.iso

# MicroPython py/ core files use relaxed flags
kernel/micropython/py/%.o: kernel/micropython/py/%.c
	$(CC) $(MPY_CFLAGS) -c $< -o $@

kernel/micropython/shared/%.o: kernel/micropython/shared/%.c
	$(CC) $(MPY_CFLAGS) -c $< -o $@

kernel/micropython/ports/tos/%.o: kernel/micropython/ports/tos/%.c
	$(CC) $(MPY_PORT_CFLAGS) -c $< -o $@

kernel/doom/port/%.o: kernel/doom/port/%.c
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

kernel/doom/%.o: kernel/doom/%.c
	$(CC) $(DOOM_CFLAGS) -c $< -o $@

# Wolf4SDL and DOOM are two entirely separate id Software engines from
# different eras, each with its own small, generic, non-static global
# variable names (view*, states, gamestate, configdir, MainMenu, ...)
# that happen to collide once both are linked into the same
# kernel/tOS.elf -- C++ doesn't mangle plain data symbols the way it
# mangles function names, so `int viewx;` in kernel/wolf3d/wl_draw.cpp
# and `int viewx;` in kernel/doom/r_main.c are, as far as the linker
# is concerned, the exact same symbol. objcopy --redefine-sym renames
# just Wolf3D's copies with a w3d_ prefix after each object file
# compiles (a no-op for any object that doesn't happen to define or
# reference a given name, so this list is safe to apply uniformly
# across every kernel/wolf3d/ object). Found by compiling the whole
# tree and reading every "multiple definition" error the linker
# reported -- if a future source change introduces a new colliding
# global, the same error will point at exactly which name to add here.
WOLF_REDEFINE_SYMS = --redefine-sym rndindex=w3d_rndindex \
	--redefine-sym states=w3d_states \
	--redefine-sym finetangent=w3d_finetangent \
	--redefine-sym viewcos=w3d_viewcos \
	--redefine-sym viewsin=w3d_viewsin \
	--redefine-sym viewangle=w3d_viewangle \
	--redefine-sym viewy=w3d_viewy \
	--redefine-sym viewx=w3d_viewx \
	--redefine-sym gamestate=w3d_gamestate \
	--redefine-sym demoname=w3d_demoname \
	--redefine-sym centerx=w3d_centerx \
	--redefine-sym viewheight=w3d_viewheight \
	--redefine-sym viewwidth=w3d_viewwidth \
	--redefine-sym configdir=w3d_configdir \
	--redefine-sym menuactive=w3d_menuactive \
	--redefine-sym MainMenu=w3d_MainMenu \
	--redefine-sym demobuffer=w3d_demobuffer \
	--redefine-sym demoplayback=w3d_demoplayback

kernel/wolf3d/port/%.o: kernel/wolf3d/port/%.c
	$(CC) $(WOLF_CFLAGS) -c $< -o $@
	objcopy $(WOLF_REDEFINE_SYMS) $@

kernel/wolf3d/%.o: kernel/wolf3d/%.c
	$(CC) $(WOLF_CFLAGS) -c $< -o $@
	objcopy $(WOLF_REDEFINE_SYMS) $@

kernel/wolf3d/%.o: kernel/wolf3d/%.cpp
	$(CXX) $(WOLF_CXXFLAGS) -c $< -o $@
	objcopy $(WOLF_REDEFINE_SYMS) $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

kernel/audio/demo_song_embed.o: kernel/audio/demo_song_embed.s kernel/audio/demo_song.wav
	$(AS) $(ASFLAGS) $< -o $@

kernel/audio/demo_song.wav: gen_demo_song.py
	python3 gen_demo_song.py || true

kernel/boot/boot.o: kernel/boot/boot.s
	$(CC) $(CFLAGS) -x assembler-with-cpp -c $< -o $@

kernel/tOS.elf: $(KERNEL_OBJS)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS)

initrd.tar: $(PROGRAMS)
	tar cf $@ --format=ustar $^

tOS.iso: kernel/tOS.elf initrd.tar
	mkdir -p iso/boot/grub
	cp kernel/tOS.elf iso/boot/
	cp initrd.tar iso/boot/
	cp boot/grub/grub.cfg iso/boot/grub/
	PATH="/tmp/xorriso_ubuntu/usr/bin:$$PATH" \
	LD_LIBRARY_PATH="/tmp/xorriso_ubuntu/usr/lib/x86_64-linux-gnu:$$LD_LIBRARY_PATH" \
	grub-mkrescue -o $@ iso

run: tOS.iso
	qemu-system-x86_64 -cdrom tOS.iso -m 1024M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw \
		-serial stdio 2>/dev/null || \
	qemu-system-x86_64 -cdrom tOS.iso -m 1024M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw

run-audio: tOS.iso
	qemu-system-x86_64 -cdrom tOS.iso -m 1024M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw \
		-soundhw sb16 -serial stdio 2>/dev/null || \
	qemu-system-x86_64 -cdrom tOS.iso -m 1024M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw \
		-soundhw sb16

run-noinstall: tOS.iso
	rm -f /tmp/tfs_test.img
	dd if=/dev/zero of=/tmp/tfs_test.img bs=1M count=64 >/dev/null 2>&1
	qemu-system-x86_64 -cdrom tOS.iso -m 1024M \
		-drive file=/tmp/tfs_test.img,if=ide,format=raw \
		-serial stdio 2>/dev/null || true

clean:
	rm -f $(KERNEL_OBJS) kernel/tOS.elf initrd.tar tOS.iso
	rm -rf iso
	find kernel -name '*.d' -delete

# Real header dependency tracking (-MMD -MP on every *_CFLAGS above emits
# a .d file per object, listing every header it actually included) --
# replaces the old hand-maintained "$(CORE_C_OBJS): kernel/core/version.h"
# rule, which only ever covered version.h and nothing else. Without this,
# changing a widely-included header (e.g. adding a field to scheduler.h's
# task_t) leaves every .o that wasn't itself edited stale: they keep
# linking against the old struct layout since make has no way to know
# they depend on it, which is exactly how mouse.o/ps2.o went two months
# without rebuilding despite scheduler.h changing today. The "-" prefix
# means make doesn't complain on a clean checkout, before any .d files
# exist yet.
-include $(KERNEL_OBJS:.o=.d)
