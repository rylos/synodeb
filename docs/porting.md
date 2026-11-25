# Adding a model

A Synology model is supported once synodeb knows its hardware: what boots it, where its bays,
console and front-panel microcontroller are, and which controllers drive which LEDs and fans.
These facts come from the unit itself, read under DSM before anything is changed, and later
from the synodeb kernel booting on it. This page lists what is needed and where each piece
comes from. You don't need to write kernel code to help: a hardware report from a model nobody
has measured yet is the first step.

## Support tiers

| tier | meaning | `synodeb-setup.sh` |
|---|---|---|
| `wip` | board file written from the hardware report, not booted yet, or only partly working | refuses without `--experimental` |
| `community` | installed and tested on the hardware by an owner, boot log in the repository | installs |
| `supported` | fully tested by a maintainer, kept working on every release | installs |

A new model starts in `wip`. Until then `synodeb-setup.sh install` stops with "this NAS (...)
is not supported": that is the intended guard, don't work around it.

## 1. Hardware report under DSM

Open a [hardware report](https://github.com/rylos/synodeb/issues/new?template=hardware-report.yml)
and attach the output of `tools/collect-report.sh`, run as root under DSM with the unit as it
is (read-only: it changes nothing and mounts nothing). Rebooting first keeps the start of the
boot log in the report. What each part of it is used for:

| in the report | used for |
|---|---|
| `syno_hw_version` | `MODEL` in the board file: `synodeb-setup.sh` finds the board file by it under DSM |
| DMI vendor and product name | under Debian the board file and the kernel's model table are chosen by them; placeholder strings (e.g. `Insyde`/`MohonPeak` on Avoton boards), or no DMI at all when the boot loader does not pass the EFI tables, mean the model has to be told apart by its PCI devices instead (`PCI_DEVICE`: all of them present) |
| CPU model, `lspci -nn` | `PLATFORM` (one platform per SoC family, `platforms/<platform>/`) |
| kernel command line | `ihd_num` is `BAYS`; `SataPortMap`, `DiskIdxMap` and the DSM GPIO parameters are the first guess at the bay map |
| `scsi_host` paths, disk → ATA port list | the bay map: which controller and port each bay is (confirm by moving one disk across empty bays, level 3 of the form) |
| boot flash (`synoboot`) section | `DOM_LOADER`, `DOM_P2_FS`, `DOM_FAT_ID`, `DOM_EXT_UUID`, `DOM_P1_MIN`, `DOM_P2_MIN`: `synodeb-setup.sh` checks them before it writes the boot flash |
| serial ports, `console=` in the boot log | `CONSOLE` in the board file; which UART the front-panel microcontroller is on (synohwd, udev rule) |
| LEDs, hwmon, GPIO, i2c | disk LED controller, fan tachometers, temperature sensors, bay power and presence pins |

On AMD Ryzen models `tools/board-map.sh` (also read-only) finds the bay power, bay presence and
fan tachometer pins by reading the GPIO states while you hot-plug a disk into an empty bay. On
other platforms DSM often does not expose the GPIO controller; those pins are then measured
under Linux.

## 2. The boot flash

synodeb boots from the internal USB boot flash (the DOM) through Synology's own boot loader: it
adds two entries to its configuration and copies the Debian kernel and initrd next to DSM's.
The layout differs between generations, so a new model needs, besides the report, the file
listing below. Two generations are known:

| | DS920+, DS923+, DS423+, DS1522+ | Avoton (DS415+, DS2415+) |
|---|---|---|
| partitions | GPT or MBR, synoboot1 FAT, synoboot2 ext2 | MBR from sector 63, both FAT |
| volume ids | the same on every unit (`DOM_FAT_ID`, `DOM_EXT_UUID`) | different per unit: left empty, checked by structure |
| `SynoBootLoader.conf` | GRUB 2 syntax (`menuentry`, `set default='N'`): `DOM_LOADER="grub2"` | GRUB legacy syntax (`title`, `root (hd0,1)`, `kernel`, `default N`): `DOM_LOADER="grub-legacy"` |

A layout that fits neither needs changes to `synodeb-setup.sh`, `synodeb-dom`, the kernel
trial in the initramfs and the installer: say so in the report.


```sh
# under DSM, read-only; keep the image off the NAS
sudo dd if=/dev/synoboot of=~/synoboot-backup.img bs=1M
sha256sum ~/synoboot-backup.img
mkdir -p /tmp/p1 /tmp/p2
sudo mount -o ro /dev/synoboot1 /tmp/p1; sudo mount -o ro /dev/synoboot2 /tmp/p2
sudo find /tmp/p1 /tmp/p2 -type f -exec ls -l {} +
sudo umount /tmp/p1 /tmp/p2
```

Post the file listing and the boot loader configuration (`*.conf`, `grub.cfg`) with the serial
number and MAC addresses removed. Don't attach the image or any file from the boot flash: they
are Synology's.

## 3. What goes into the repository

- `boards/<model>.conf`: the board file (copy one of the same platform; `MODEL`, `PLATFORM`,
  `TIER`, `BAYS`, `CONSOLE`, `DOM_*`). Where the DMI product name is a placeholder,
  `DMI_PRODUCT` and `PCI_DEVICE` tell the installed system which board file is its own. `tools/sync-boards.py` copies it into `synodeb-setup.sh`
  and the package.
- `platforms/<platform>/README.md`: the hardware notes, each fact marked as measured on a
  unit, or inferred.
- The kernel: a table for the model in `kernel/src/drivers/platform/x86/synology_nas.c` (bay
  power and presence, USB power, fan tachometers, RTC, microcontroller power-off), drivers for
  hardware the mainline kernel lacks under `platforms/<platform>/kernel/src`.
- synohwd: bay count, fan curves, and the microcontroller's serial port (udev rule
  `/dev/synology-up` in `pkg/synodeb/etc/udev/rules.d/60-synodeb.rules`).

Hardware facts are welcome; please leave out references to DSM binaries or Synology code, and
don't add firmware images or files extracted from them.

## 4. First boot and promotion

Install with `synodeb-setup.sh install --experimental`: the DSM entry stays on the boot flash,
so `synodeb-dom default dsm` (or the boot loader menu) brings DSM back. A serial console helps a
lot on the first boots; the report shows where DSM puts its console. Once it boots, attach the
kernel log of the synodeb kernel to the hardware report (`journalctl -k -b`, level 2 of the
form) with a list of what works and what does not. A model with that boot log and its basics
working (boot, network, bays, power-off, fans) moves to `community`.
