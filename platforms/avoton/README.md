# Platform avoton (Intel Atom C2000, Avoton)

Models: DS415+ (work in progress, not booted yet). Board file: `boards/ds415plus.conf`.
Hardware report: [#16](https://github.com/rylos/synodeb/issues/16). Each fact is marked as
measured on a unit (under DSM 7.1.1) or inferred.

## DS415+

- **CPU**: Atom C2538, stepping B0 (`00:00.0` revision 02), affected by the AVR54 erratum (a
  clock on the board that stops, the unit no longer starts; repairable). Measured.
- **Identity under Debian**: the DMI names are placeholders, `Insyde` / `MohonPeak` /
  `Type2 - Board Product Name1`, the same as on the DS2415+. The DS415+ has a SiI3132 SATA
  controller (`1095:3132`, `01:00.0`), the DS2415+ Marvell 88SE92xx: `/usr/lib/synodeb/board`
  chooses `ds415plus.conf` by `DMI_PRODUCT` and `PCI_DEVICE` (Avoton host bridge `8086:1f0c`
  and the SiI3132). Measured (DS415+ report, DS2415+ from its owner). A synodeb kernel booted
  on the DS2415+ logged `DMI not present or invalid` and no EFI tables (#9; how it was booted
  is not known yet): without DMI the board file is chosen by `PCI_DEVICE` alone, and so is the
  udev rule of the microcontroller port.
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
  ttyS0; the udev rule names ttyS1 `/dev/synology-up` when the board file is the DS415+ one. That the microcontroller speaks the same protocol as
  on the other models is inferred, not tested. The console header is reached through the
  vent below the power button with the unit on its side (photo in #16); it looks like the
  2×3 header of the DS923+ ([serial console](../../docs/serial.md)). The DS414 (ARM, another
  board) uses the same header the same way: the row closest to the board, pins 2-4-6, with NAS RX
  closest to the power button
  ([forum post](https://forum.doozan.com/read.php?3,100059)). On the DS415+ that is inferred,
  not measured.
- **Network**: two onboard ports. Under DSM `macs=` comes from the boot loader (`vender`); the
  NICs themselves carry a placeholder address (reported by luuvasara, #7), so until the kernel
  sets the right ones, both ports may come up with the same MAC address.
- **LEDs**: `syno_led0`-`15` under DSM, a 16-output LP3943 (inferred from other owners' reports,
  #7). No driver here yet.

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

Open: whether this boot loader starts a current kernel (bzImage, EFI) with a Debian initrd at
all. Only the first boot with `synodeb-setup.sh install --experimental` will tell. The DSM
entries stay on the DOM, and once the installer runs it makes SYNOLOGY_2 the default again
(any reboot before the end brings DSM back). If the boot loader cannot load the installer
files, it goes to its `fallback` entry, SYNOLOGY_1 (Synology's own setting). If it loads them
but the kernel never starts, the unit keeps booting the INSTALLER entry: DSM then comes back
only through the boot loader menu, if a serial console reaches it (its `serial --unit=1` is
ttyS1, the microcontroller's port: not checked), or by writing the DOM backup from another
system. That is the risk of the first test.

## Not done yet

- kernel: no table in `synology_nas.c` (bay power, presence, fans, power-off) and no MAC address
  fix for the onboard ports;
- synohwd: the bay map above is in (from the next synodeb release), no fan curves (it falls back
  to the DS923+ ones);
- disk LEDs (LP3943).
