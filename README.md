# ArmtOS

ArmtOS is [tOS](https://github.com/Artfical/tOS) on ARM: the same from-scratch operating
system, brought to the Raspberry Pi. It keeps tOS's history and its portable code — the
filesystems, the network stack, the TLS client, the shell — and replaces everything that
talked to a PC: the boot path, the console, the interrupt controller and the drivers.

Every push to `main` is built, booted in an emulated Raspberry Pi, and published as a
card image you can write to an SD card. See **[Releases](../../releases)** — each one is
named `build @<commit>`.

## Trying it

**On real hardware.** Download `armtos-<commit>.img.gz` from the latest release and write
it to a card:

```
gunzip -c armtos-<commit>.img.gz | sudo dd of=/dev/sdX bs=4M conv=fsync
```

Put the card in a Pi 2, 3 or 4, connect a USB-serial adapter to the header (GPIO 14 is
TX, GPIO 15 is RX, ground to ground) and open it at **115200 baud**. The board prints
what it found and gives you a prompt.

**In an emulator**, with no hardware at all:

```
make -f Makefile.arm64
qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio -display none
```

## What it does so far

```
  ArmtOS 0.9.178 -- tOS on ARM
  board: Raspberry Pi 3 (BCM2837)
  peripherals at 0x000000003F000000
  board revision: 0x0000000000A02082
  memory: 960 MB

[OK] early boot reached C
ArmtOS> usb
2 device(s)
  addr 1 vendor 0x409 product 0x55AA class 0x9
  addr 2 vendor 0x46F4 product 0x1 class 0x0
```

| Area | State |
|---|---|
| **Boot** | The firmware's entry at `0x80000`, the other three cores parked, a drop from EL2 to EL1, BSS cleared, C reached (`boot.S`). |
| **Board detection** | Pi 2, 3 and 4 tell themselves apart by their core ID, and the peripheral base follows, so one image serves all three (`bcm.c`). |
| **Console** | PL011 UART on GPIO 14/15 at 115200 baud (`uart.c`). |
| **Firmware mailbox** | Board revision, memory size, and a framebuffer request (`mailbox.c`). |
| **GPIO** | Function select, pull-up/down (both the old clocked protocol and the Pi 4's register), read and write (`gpio.c`). Try `gpio 21 1` and `read 21`. |
| **USB 2.0 host** | DWC2 in host mode: port reset, control and bulk transfers, device addressing, and a walk of the hub's ports — which matters because every Pi has a hub on the board and everything else sits behind it (`dwc2.c`). Storage and network devices are both found. |
| **Shell** | A small serial shell: `help`, `board`, `mem`, `gpio`, `read`, `usb`, `reboot`. |

## What is not there yet

This is an honest list, not a roadmap of intentions:

- **Ethernet.** The port on a Pi 3 is a SMSC LAN9514 behind the USB hub, and on a Pi 4 it
  is the memory-mapped GENET block. The USB groundwork underneath is working and tested,
  but neither chip is emulated by any QEMU available here, so no driver for them has been
  written: shipping one that has never moved a packet would be a claim, not a feature.
- **USB 3.** On a Pi 4 that is a VL805 controller behind PCIe, which again no emulator
  here provides.
- **Storage.** The SD card is not read yet, so there is no filesystem; tOS's own
  filesystem code is in the tree but not wired to a block device on this architecture.
- **Interrupts, timers and scheduling.** Everything is polled, and only one core runs.
- **Memory management.** The MMU is off, so there are no address spaces and no user mode
  — tOS's processes do not run here yet.
- **Graphics.** The mailbox can ask for a framebuffer, but nothing draws to it.

## How a commit becomes an image

`.github/workflows/release.yml` runs on every push:

1. builds `kernel8.img` with the AArch64 cross compiler,
2. boots it under `qemu-system-aarch64 -M raspi3b` and **fails the build unless the kernel
   reaches C** — a release that cannot boot is never published,
3. fetches the Raspberry Pi firmware the GPU needs,
4. builds an MBR card image with a FAT32 boot partition (`tools/image/mkimage.sh`),
5. publishes it as `build @<commit>`.

## Layout

```
kernel/arch/arm64/     the ARM port: boot.S, bcm.c, uart.c, mailbox.c, gpio.c, dwc2.c
tools/image/           mkimage.sh, which turns kernel8.img into a bootable card image
Makefile.arm64         the ARM build
Makefile               the original x86 build, untouched
kernel/fs, kernel/net  tOS's portable code, waiting to be wired up here
```

## Licence and origin

Forked from [Artfical/tOS](https://github.com/Artfical/tOS), with its history intact.
