#!/bin/bash
# Boots kernel8.img in QEMU with a USB Ethernet adapter behind the emulator's user-mode
# network (real internet access), and runs the shell's `nettest` command.
#   tools/test/nettest.sh [raspi3b|raspi4b] [seconds]
# QEMU's raspi4b models neither the Pi 4's Ethernet controller nor its PCIe/USB 3, but it does
# model the DWC2 USB host, so the whole stack above the driver is tested the same way on both.
set -e
cd "$(dirname "$0")/../.."
MACHINE=${1:-raspi3b}
SECS=${2:-90}
WSPORT=18080
python3 tools/test/wsecho.py $WSPORT & WS=$!
trap 'kill $WS 2>/dev/null' EXIT
truncate -s 64M /tmp/nettest.img
( sleep 9; printf 'nettest %s\n' $WSPORT; sleep "$SECS" ) | \
  timeout $((SECS + 20)) qemu-system-aarch64 -M "$MACHINE" -kernel kernel8.img \
    -serial stdio -display none -drive file=/tmp/nettest.img,if=sd,format=raw \
    -device usb-net,netdev=n0 -netdev user,id=n0 2>&1 | tr -d '\r' | tee /tmp/nettest.log || true
grep -q "failed" /tmp/nettest.log && ! grep -q " 0 failed" /tmp/nettest.log && exit 1
grep -q " 0 failed" /tmp/nettest.log
