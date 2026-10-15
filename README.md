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

A Pi 4 needs QEMU 7.2 or newer (`-M raspi4b`); the 6.2 that Ubuntu 22.04 ships has no such
machine. Both boards are checked on every commit.

**Writing a program.** `sdk/armtos-cc` builds one, and `sdk/examples/` has three:

```
./sdk/armtos-cc sdk/examples/blink.c -o blink.t
mcopy -i armtos.img@@1M blink.t ::BLINK.T       # onto the card, beside kernel8.img
```

then `run blink.t` on the board. The same hardware is reachable from Python:

```python
import pi
pi.pin_mode(21, pi.OUTPUT)
pi.write(21, 1)
pi.analog_write(18, 512)        # PWM
print(pi.i2c_scan())
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
| **GPIO extras** | PWM on GPIO 12, 13, 18 and 19 — the closest a Pi has to an analog output, since there is no DAC. I2C on GPIO 2/3 with a bus scan, and SPI on GPIO 7-11, which is how an external ADC is reached: the board cannot measure a voltage itself (`pwm.c`, `i2c.c`, `spi.c`). |
| **Programs** | A `.t` binary is read from the card's FAT partition, loaded at a fixed address, and run at EL0 in its own window of memory. It reaches the hardware through system calls, and a fault ends the program rather than the board (`loader.c`, `syscall_arm.c`, `fat.c`). |
| **Python** | MicroPython built for AArch64, with a `pi` module covering every pin, bus and the display. `py print(2**100)` and `python` for a prompt. |
| **Network stack** | The x86 tOS TCP/IP stack, compiled unchanged for the board: ARP, IPv4/IPv6, ICMP, UDP, TCP, DHCP, DNS, HTTP, TLS (ECDHE, AES-GCM, ChaCha20-Poly1305, certificate chains checked), WebSocket and SNTP. Shell: `net`, `ping`, `dns`, `ntp`, `wget`, `https`, `ws`, `nettest`. Tested in QEMU against the real internet on **both `raspi3b` and `raspi4b`** (`tools/test/nettest.sh`) through a USB CDC-ECM adapter — QEMU models no Pi Ethernet controller, so that is what carries the packets. |
| **Ethernet** | GENET for the Pi 4's built-in controller, and the LAN95xx that sits on the USB bus of a Pi 2/3, behind one interface that picks whichever the board has (`net.c`). **Untested** — see below. |
| **Shell** | A serial shell: `help`, `board`, `mem`, `uptime`, `heap`, `gpio`, `read`, `pwm`, `i2cscan`, `adc`, `usb`, `sd`, `ls`, `run`, `fb`, `eth`, `py`, `python`, `reboot`. |

## What is not there yet

This is an honest list, not a roadmap of intentions:

- **Ethernet is written but never tested.** The drivers for both chips exist — GENET for a
  Pi 4, LAN95xx over USB for a Pi 2/3 — but no QEMU emulates either, including 9.1 with its
  `raspi4b` machine, which models only the USB controller. The GENET driver was rewritten
  against Linux's `bcmgenet` register map (a first version used the wrong base address and
  never enabled the DMA engine), but a driver that has never touched the silicon is still a
  guess: on a real Pi 4, `eth` prints the controller's revision register, the PHY it found and
  the negotiated link, which is what to report if it fails. `eth` says plainly when it finds
  nothing rather than pretending.
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
                       mmu.c, irq.c, heap.c, gpio.c, pwm.c, i2c.c, spi.c, dwc2.c,
                       emmc.c, fat.c, fb.c, net.c, genet.c, lan95xx.c, hwapi.c,
                       syscall_arm.c, loader.c
kernel/micropython/ports/armtos/   the Python port: arm_hal.c, arm_mp.c, modpi.c
sdk/                   armtos-cc, armtos.h and examples for writing .t programs
tools/image/           mkimage.sh, which turns kernel8.img into a bootable card image
Makefile.arm64         the ARM build
Makefile               the original x86 build, untouched
kernel/fs, kernel/net  tOS's portable code, waiting to be wired up here
```

## Licence and origin

Forked from [Artfical/tOS](https://github.com/Artfical/tOS), with its history intact.
