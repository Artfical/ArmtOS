#!/bin/bash
# Builds a bootable Raspberry Pi SD-card image from kernel8.img.
#
# A Pi boots from a FAT32 partition: the GPU reads bootcode/start.elf from it, then loads
# the file named in config.txt. So the image is one MBR with a single FAT32 partition
# holding the firmware and our kernel -- write it to a card with dd and the Pi starts it.
#
# usage: mkimage.sh OUT.img [FIRMWARE_DIR]
set -e
out=${1:-armtos.img}
fw=${2:-firmware}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

[ -f "$root/kernel8.img" ] || { echo "kernel8.img not built"; exit 1; }

size_mb=64
rm -f "$out"
truncate -s ${size_mb}M "$out"

# One bootable FAT32 partition starting at 1 MiB (the usual alignment).
parted -s "$out" mklabel msdos
parted -s "$out" mkpart primary fat32 1MiB 100%
parted -s "$out" set 1 boot on

offset=$((1024 * 1024))
part="$out.part"
truncate -s $((size_mb * 1024 * 1024 - offset)) "$part"
mkfs.vfat -F 32 -n ARMTOS "$part" > /dev/null

# config.txt tells the firmware to start our image in 64-bit mode on all four cores, and
# to bring the PL011 up on the header pins so the serial console works from the first line.
cat > /tmp/config.txt <<CFG
arm_64bit=1
kernel=kernel8.img
enable_uart=1
uart_2ndstage=1
disable_commandline_tags=1
core_freq_min=500
# I2C and SPI on, so the header works out of the box; `raspi-config` edits these lines
dtparam=i2c_arm=on
dtparam=spi=on
CFG
mcopy -i "$part" /tmp/config.txt ::config.txt
mcopy -i "$part" "$root/kernel8.img" ::kernel8.img

# The GPU firmware itself is not ours to ship from source; when a copy is available it is
# included so the image boots unaided, and otherwise the image still works on a card that
# already has it.
if [ -d "$fw" ]; then
    for f in "$fw"/*; do [ -f "$f" ] && mcopy -i "$part" "$f" "::$(basename "$f")"; done
    echo "firmware included from $fw"
else
    echo "note: no firmware directory -- copy bootcode.bin/start*.elf/fixup*.dat onto the card yourself"
fi

dd if="$part" of="$out" bs=1M seek=1 conv=notrunc status=none
rm -f "$part"
echo "wrote $out ($(du -h "$out" | cut -f1))"
