# Platform avoton (Intel Atom C2000, Avoton)

Models: DS415+ (work in progress: installer reached, bays not yet tested). Board file: `boards/ds415plus.conf`.
Hardware report: [#16](https://github.com/rylos/synodeb/issues/16). Each fact is marked as
measured on a unit (under DSM 7.1.1) or inferred.

## DS415+

- **CPU**: Atom C2538, stepping B0 (`00:00.0` revision 02), affected by the AVR54 erratum (a
  clock on the board that stops, the unit no longer starts; repairable). Measured.
- **Identity under Debian**: the DMI names are placeholders, `Insyde` / `MohonPeak` /
  `Type2 - Board Product Name1`, the same as on the DS2415+. The DS415+ has a SiI3132 SATA
  controller (`1095:3132`, `01:00.0`), the DS2415+ Marvell 88SE92xx: `/usr/lib/synodeb/board`
  chooses `ds415plus.conf` by `DMI_PRODUCT` and `PCI_DEVICE` (Avoton host bridge `8086:1f0c`
  and the SiI3132). Measured (DS415+ report, DS2415+ from its owner). The BIOS publishes
  SMBIOS only through the EFI tables (nothing in the legacy area at `0xF0000`), so a kernel
  without EFI has no DMI at all: that happened on the DS2415+ when the synodeb kernel was
  started through a `noefi` kernel and kexec (#9, #16). Without DMI the board file is chosen
  by `PCI_DEVICE` alone, and so is the udev rule of the microcontroller port. Measured on the
  DS2415+.
- **Model under DSM**: `/proc/sys/kernel/syno_hw_version` is `DS415+-j`, the kernel command line
  says `syno_hw_version=DS415+`; `synodeb-setup.sh` reads the command line first. Measured.
- **Bays**: `SataPortMap=422 DiskIdxMap=020006`. Measured with a known disk in each bay:

  | bay | controller | port | under DSM |
  |---|---|---|---|
  | 1 | `00:18.0` AHCI, 2 ports, 6 Gbps | 0 | `ata5` |
  | 2 | `00:18.0` | 1 | `ata6` |
  | 3 | `00:17.0` AHCI, 4 ports, 3 Gbps | 0 | `ata1` |
  | 4 | `00:17.0` | 1 | `ata2` |

  Ports 2 and 3 of `00:17.0` and both SiI3132 ports had no disk; that the SiI3132 drives the
  eSATA port is inferred. The `ataN` numbers are DSM's probe order and may differ under Debian:
  the map is by PCI device and port.
- **Serial**: DSM's console on ttyS0 (I/O 0x3f8), its microcontroller driver opens ttyS1
  (I/O 0x2f8, 9600 baud, measured with `stty`). The board file puts the kernel console on
  ttyS0; the udev rule names ttyS1 `/dev/synology-up` when the board file is the DS415+ one. The microcontroller takes the same commands as on the
  other models (9600 8N1, see below); not yet tested from Debian. The console header is reached through the
  vent below the power button with the unit on its side (photo in #16); it looks like the
  2×3 header of the DS923+ ([serial console](../../docs/serial.md)). The DS414 (ARM, another
  board) uses the same header the same way: the row closest to the board, pins 2-4-6, with NAS RX
  closest to the power button
  ([forum post](https://forum.doozan.com/read.php?3,100059)). On the DS415+ that is inferred,
  not measured.
- **Network**: two onboard ports (Intel igb, `00:14.0` and `00:14.1`). Under DSM `macs=` comes
  from the boot loader (`vender`); the NICs themselves carry a placeholder address (reported by
  luuvasara, #7). The kernel gives them the `macs=` addresses in PCI order (`00:14.0` the first,
  `00:14.1` the second); that this is DSM's LAN 1 / LAN 2 order is inferred.

## What the synodeb kernel and synohwd do on the DS415+

From the next kernel release (not yet checked on a unit):

- **Model**: the platform driver `synology_nas` takes the DS415+ by the Avoton host bridge
  (`8086:1f0c`) with the SiI3132 (`1095:3132`), when the DMI name is `MohonPeak` or missing.
- **Bays**: power on GPIO 10, 15, 16, 17 (active high), presence on GPIO 18, 28, 34, 44 (active
  low), the SoC's legacy GPIO block (`gpio_ich`, built into the kernel with `lpc_ich`). Disks are
  powered one at a time, 5 s apart, then 3 s before the SATA probe; hot-plug like the other
  models; off one by one at power-off. Without this the bays stay unpowered and no disk is seen.
- **Disk LEDs**: a TI LP3943 at 0x60 on the SMBus of the SoC (`i2c-i801`), output 2n green and
  2n+1 orange of bay n+1, wired active low; GPIO 49 enables them. Driver
  `leds-synology-lp3943` (`syno:green:diskN`, `syno:orange:diskN`), driven by synohwd.
- **Fans**: tachometers on GPIO 11 and 12 (`fan1`, `fan2` of hwmon `synology`, sampled); speed
  set by synohwd through the microcontroller (`V<duty>`), with the DS415+ curves (disks
  0/37/41/48/53 °C → 21/35/50/70/99 % in cool mode, 0/42/46/53/58 °C in quiet mode, shutdown
  at 61 °C; CPU 0/50/85 °C → 21/50/99 %, shutdown at 95 °C).
- **Microcontroller**: ttyS1 (I/O 0x2F8), 9600 8N1 raw, no flow control (`/dev/synology-up`).
  Restart sends `C`, power-off `1`, as on the other models; power LED, beeps, status LED and the
  power button use the same commands.
- **RTC**: the CMOS RTC (`rtc_cmos`, rtc0), loaded only on this model; the system clock is set
  from it at boot (UTC). Scheduled power-on: its wake alarm (`rtcwake -d rtc0`), and synohwd
  tells the microcontroller at power-off.
- **USB**: no VBUS switch, the ports are always powered.

## Boot flash (DOM)

Measured (#16): 120 MiB, MBR, synoboot1 FAT16 from sector 63 (32067 sectors), synoboot2 FAT16
from sector 32130 (192780 sectors), with volume ids of their own on every unit.

- synoboot1: `EFI/boot/SynoBootLoader.conf` and `SynoBootLoader.efi`, `GRUB_VER`, the factory
  `zImage` and `rd.gz`.
- synoboot2: the DSM kernel as updated by DSM (`zImage`, `rd.gz`), `vender`,
  `grub_cksum.syno`, `checksum.syno`; about 85 MB free.
- `GRUB_VER`: `GRUB_PROJECT="grub-0.97-efi"`, `GRUB_VERSION="1"`: GRUB legacy 0.97, EFI build.
- `SynoBootLoader.conf` is in GRUB legacy syntax: `default 1` (SYNOLOGY_2, the kernel on
  synoboot2), `fallback 0` (SYNOLOGY_1, the factory one), `serial --unit=1`, entries
  `title` / `root (hd0,N)` / `kernel` / `initrd`; SYNOLOGY_2 also has `cksum /grub_cksum.syno`
  and `vender /vender show`.

synodeb writes its entries in the same syntax (`DOM_LOADER="grub-legacy"`):

```
title DEBIAN
	root (hd0,1)
	vender /vender show
	kernel /synodeb/vmlinuz root=... synodeb.entry=DEBIAN
	initrd /synodeb/initrd.img
```

and only one `fallback` number (whether this build takes a list is not known). FAT has no hard
links, so a kernel update copies the previous kernel to DEBIAN_OLD instead of linking it.

The boot loader starts a current kernel itself, and passes it the EFI tables. On the DS2415+,
from the boot flash, with no `noefi` and no kexec (measured by its owner, serial logs, #16):
- Debian's 6.12 kernel (`6.12.113+deb13-amd64`);
- the synodeb kernel `6.18.60-5-ds923` with a small initramfs: `efi: EFI v2.4 by INSYDE Corp.`,
  `SMBIOS 2.7 present`, `DMI: Insyde MohonPeak/...`, `/init` running after 4.8 s, no oops;
  `/sys/firmware/efi` present, `product_name` `MohonPeak`.

Same `grub-0.97-efi` build as the DS415+; on the DS415+ itself it is inferred. The `vender`
line appends `macs=` and `sn=` to whatever kernel command line the entry gives.

**Boot loader menu on the console.** On the DS2415+ GRUB's `Press Ctrl-C to enter the menu`,
the menu and the `grub>` prompt appear on the console port, the 16550 at `0x3F8` (Linux's
ttyS0), although the configuration says `serial --unit=1` (measured). In the EFI build of GRUB
legacy the unit is a position in the firmware's list of serial devices, not the BIOS port
number, so unit 1 is `0x3F8` there. On the DS415+ the same is likely (same build), but not
measured: if it holds, a USB-UART adapter on the console header reaches the menu and can
always pick a DSM entry. GRUB drops characters typed faster than about 60 ms apart; the BIOS
prints nothing usable before GRUB.

Open: the first boot with `synodeb-setup.sh install --experimental`. The DSM entries stay on
the DOM, and once the installer runs it makes SYNOLOGY_2 the default again (any reboot before
the end brings DSM back). If the boot loader cannot load the installer files, it goes to its
`fallback` entry, SYNOLOGY_1 (Synology's own setting). If it loads them but the kernel never
starts, the unit keeps booting the INSTALLER entry: DSM then comes back through the boot
loader menu on the serial console (see above), or by writing the DOM backup from another
system.

## Not done yet

- everything above still has to be checked on a unit (the first boots, #16, reached the
  installer but saw no disk: the bays were not powered);
- front LED dimming (no table for this model);
- `pwrctl_pin=N0754` on the DSM command line (power control of expansion units): not used.
