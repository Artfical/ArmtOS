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
[OK] MMU on, caches enabled

[OK] early boot reached C
[OK] interrupts and timer running
ArmtOS> sd
card: 64 MB
  partition 1: type 0xC start 2048 size 63 MB
ArmtOS> usb
2 device(s)
  addr 1 vendor 0x409 product 0x55AA class 0x9
  addr 2 vendor 0x525 product 0xA4A2 class 0x2
ArmtOS> uptime
7.742 s, 773 ticks
```

Everything above is what the emulator actually prints; the screen output is checked the
same way, by taking a screendump of the framebuffer.

| Area | State |
|---|---|
| **Boot** | The firmware's entry at `0x80000`, the other three cores parked, a drop from EL2 to EL1, BSS cleared, C reached (`boot.S`). |
| **Board detection** | Pi 2, 3, 4 and 5 tell themselves apart by their core ID, and the peripheral base, console address and interrupt controller all follow from it, so one image serves them all (`bcm.c`). |
| **Console** | PL011 UART at 115200 baud — on GPIO 14/15, or on the Pi 5's debug connector, whose UART is at a different address entirely (`uart.c`). |
| **Firmware mailbox** | Board revision, memory size, and framebuffer allocation (`mailbox.c`). |
| **MMU and caches** | A flat identity map of 2 MB blocks: RAM normal and cacheable, the peripheral window device memory, so register writes are neither cached nor reordered (`mmu.c`). |
| **Interrupts and time** | Exception vectors that report a fault instead of hanging, and a 100 Hz tick from the ARM generic timer, whose rate is read from the core rather than assumed. Broadcom's controller on a Pi 2/3, the GIC on a Pi 4/5 (`irq.c`, `exception.S`). |
| **GPIO** | Function select, pull-up/down (both the old clocked protocol and the Pi 4's register), read and write (`gpio.c`). Try `gpio 21 1` and `read 21`. |
| **USB 2.0 host** | DWC2 in host mode: port reset, control and bulk transfers, device addressing, and a walk of the hub's ports — which matters because every Pi has a hub on the board and everything else sits behind it (`dwc2.c`). Storage and network devices are both found. |
| **SD card** | The host controller brought up through the full identification sequence, blocks read and written, and the card's size taken from its own CSD (`emmc.c`). The partition table of a card is read and listed. |
| **Display** | A framebuffer from the firmware, with rectangles, lines and an 8x8 font — and the pixel order taken from what the firmware actually set, not what was asked for (`fb.c`). |
| **Shell** | A serial shell: `help`, `board`, `mem`, `uptime`, `gpio`, `read`, `usb`, `sd`, `fb`, `reboot`. |

## What is not there yet

This is an honest list, not a roadmap of intentions:

- **Ethernet.** The port on a Pi 3 is a SMSC LAN9514 behind the USB hub, on a Pi 4 the
  memory-mapped GENET block, and on a Pi 5 it is behind RP1. The USB groundwork underneath
  is working and tested, but none of those chips is emulated by any QEMU available here, so
  no driver for them has been written: shipping one that has never moved a packet would be
  a claim, not a feature.
- **USB 3.** On a Pi 4 that is a VL805 controller behind PCIe, and on a Pi 5 it is in RP1 —
  again, nothing here can emulate either.
- **The Pi 5's header.** Its GPIO, USB and Ethernet all live in the RP1 companion chip
  across PCIe, which is a different driver from the ones here. The board is detected and
  its console works; the header is deliberately left alone rather than poked at an address
  that is not GPIO on that machine.
- **Filesystems.** The SD card is read, but tOS's filesystem code is not yet wired to it.
- **Scheduling.** There is a timer tick but no scheduler, and only one of the four cores
  runs.
- **User mode.** The MMU is on, but there are no separate address spaces yet, so tOS's
  processes do not run here.

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
kernel/arch/arm64/     the ARM port: boot.S, exception.S, bcm.c, uart.c, mailbox.c,
                       mmu.c, irq.c, gpio.c, dwc2.c, emmc.c, fb.c
tools/image/           mkimage.sh, which turns kernel8.img into a bootable card image
Makefile.arm64         the ARM build
Makefile               the original x86 build, untouched
kernel/fs, kernel/net  tOS's portable code, waiting to be wired up here
```

## Licence and origin

Forked from [Artfical/tOS](https://github.com/Artfical/tOS), with its history intact.
