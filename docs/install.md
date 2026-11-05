# Installation

The short version is in the [README](../README.md#quick-start). This page has the details.

## Requirements

- A supported NAS (see the [README](../README.md#supported-models)) running DSM 7.
- SSH enabled in DSM (*Control Panel → Terminal & SNMP*) and an administrator account.
- Internet access: the installer downloads Debian.
- A backup of your data. If you plan to erase the DSM disks, make sure nothing on them is
  still needed.

## Steps

1. **On DSM**, over SSH, download and run the setup script:

   ```sh
   curl -fsSLO https://raw.githubusercontent.com/rylos/synodeb/main/synodeb-setup.sh
   sudo bash synodeb-setup.sh install
   ```

   It asks for the network (`dsm` or `dhcp`), the host name and the installer language, backs
   up the DOM, asks before writing it, adds the Debian installer to it and prints how to
   connect, including a one-time password. **Copy the DOM backup off the NAS**: it is the only
   way to rebuild the DOM.

   Writing the DOM takes a minute or two. While it writes, the script ignores a dropped SSH
   session and Ctrl-C, but a session that drops earlier stops it. On an unreliable connection
   run it under `screen` or `tmux`, or detached:
   `sudo nohup bash synodeb-setup.sh install --yes > setup.log 2>&1` (add `--network`,
   `--hostname`, `--lang` as needed) and read the password in `setup.log`.
2. **Reboot** the NAS (`sudo reboot`). After 2-3 minutes it gives **one long beep**: the
   installer is ready.
3. **Connect** with the command and password the script printed (`ssh installer@<address>`),
   choose *Start installer* and follow the Debian installer. The SSH session stays up for the
   whole installation; if it drops, connect again and the installer is where you left it.
4. **Partition** (see [Choosing the disks](#choosing-the-disks)).
5. At the end the NAS reboots into Debian. Log in with the user you created:
   `ssh <user>@<address>`. With `--network dsm` Debian keeps the address DSM had; with `dhcp`
   it may get a different one from your DHCP server.

## Choosing the disks

- Any layout the Debian installer offers works: plain partitions, mdadm RAID, LVM, ext4, XFS,
  btrfs (also btrfs RAID1 across two disks or NVMe drives).
- **btrfs RAID1 for the root**: the installer creates the filesystem on one disk; add the
  second one afterwards (see [RAID1 root on two drives](#raid1-root-on-two-drives)).
- **No encrypted root** (*encrypted LVM*): it cannot be unlocked at boot without a console.
  Encrypt data volumes instead, with a key file on the root filesystem.
- **The DOM** (the 128 MB disk with two small partitions) is shown as installation media and
  locked: leave it alone, the NAS cannot boot without it. In manual partitioning the installer
  may propose its first partition (FAT) as `/boot/efi`: set it to *do not use*. The NAS does
  not boot with UEFI and needs no EFI partition.

## RAID1 root on two drives

Install on the first drive (btrfs root, Debian puts it in the subvolume `@rootfs`), then copy
the partition table to the second, empty drive and convert the filesystem to RAID1. With two
NVMe drives, the system on `nvme0n1` and the empty one `nvme1n1` (check with `lsblk` first:
everything on the second drive is erased):

```sh
sudo sfdisk -d /dev/nvme0n1 | grep -v '^label-id:' | sed 's/, uuid=[0-9A-F-]*//' | sudo sfdisk /dev/nvme1n1
sudo mkswap /dev/nvme1n1p1      # if the first drive has a swap partition: add it to /etc/fstab with nofail
sudo btrfs device add -f /dev/nvme1n1p2 /
sudo btrfs balance start -dconvert=raid1 -mconvert=raid1 -sconvert=raid1 -f /
sudo update-initramfs -u -k all
```

`sudo btrfs filesystem usage -T /` then shows RAID1 on both drives.

**A failed or missing drive**: the system keeps running when a drive dies. At boot, if a
device of the btrfs root is still missing after 60 s, the initramfs mounts the root
`degraded` and the "boot completed" notification says so with high priority. While degraded,
new data is written to the remaining drive only: replace the failed one as soon as possible
(`sudo btrfs replace start <missing devid> /dev/<new partition> /`, devid from
`sudo btrfs filesystem show /`; copy the partition table first as above). A drive that was
only disconnected and comes back holds stale copies: on the first boot with all the devices
present a scrub is started by itself to rewrite them from the good drive
(`sudo btrfs scrub status /`).

## Keeping DSM

DSM lives on its own disks (a small RAID1 system partition on every DSM disk) and on the DOM.
synodeb never changes DSM: it only adds its boot entries and files to the DOM.

To keep DSM, install Debian **on a disk DSM does not use**: an empty disk in a free bay, or an
NVMe drive that is not part of a DSM volume. An NVMe drive used as an **SSD cache** is part of
a volume too: remove the cache in DSM first (*Storage Manager*), because a read-write cache
holds data that is lost when the drive is erased. In *Partition disks* choose *Manual* or
*Guided – use entire disk* on that disk only. The installer does not assemble the DSM RAID
arrays, but it still lists the DSM disks: check the size and model of the disk you pick.

The installed Debian keeps that rule: `/etc/mdadm/mdadm.conf` lists the arrays that exist at
the end of the installation and has `AUTO -all`, so DSM's arrays are never assembled. An array
created later is assembled at boot once its line is added there: copy it from
`sudo mdadm --detail --scan`, then run `sudo update-initramfs -u`.

To use the DSM disks for Debian instead, select them in the partitioner and let it erase them:
DSM and its data on those disks are gone. DSM volumes can be read from Debian before that
(`synodeb-dsm`, see [usage](usage.md)).

## When something goes wrong

- If the installer gets no network within 8 minutes it reboots into DSM by itself. Until the
  installation completes, any reboot brings DSM back.
- If the installer cannot switch the DOM back to DSM when it starts, the status LED turns
  orange with three short beeps; after 8 minutes without SSH there are five short beeps and no
  reboot (it would start the installer again). Connect over SSH and complete the installation,
  or choose *Execute a shell* and switch the DOM by hand:
  `. /usr/lib/synodeb-installer/lib.sh; dom_unprotect; dom_default SYNOLOGY_2; dom_protect`.
- A damaged DOM is rebuilt from DSM with `sudo bash synodeb-setup.sh restore FILE` (the
  backup made by `install`).

## synodeb-setup.sh

Runs on DSM, as root. Without arguments it shows a menu; every entry is also a command.

| command | what it does |
|---|---|
| `install` | backs up the DOM and adds the Debian installer to it, made the default boot entry; the kernel of an installed Debian stays on the DOM (see `--replace-debian`) |
| `boot debian` | makes the installed Debian the default boot entry |
| `boot dsm` | makes DSM the default boot entry |
| `status` | shows the boot entries, the default one and the files of synodeb on the DOM |
| `cancel` | removes the installer from the DOM; DSM boots (an installed Debian stays: `boot debian` makes it the default again) |
| `restore FILE` | writes a DOM backup back, byte for byte, and verifies it; works on a damaged DOM too |
| `uninstall` | advanced: removes everything synodeb put on the DOM (boot entries, kernels, installer, markers) and puts the original Synology boot configuration back |

Options of `install` (they can come before or after the command):

| option | default | |
|---|---|---|
| `--network dsm\|dhcp` | asked (`dsm`, or `dhcp` if DSM uses DHCP) | `dsm`: the address DSM has now, as a static address; `dhcp`: an address from the DHCP server. With a DSM bond or Open vSwitch the installer uses one port only |
| `--hostname NAME` | asked (DSM host name) | letters, digits and `-` |
| `--lang en\|it\|de\|fr\|es\|nl\|pt` | asked (`en`) | language and keyboard layout of the installer |
| `--backup-dir DIR` | current folder | where the DOM backup goes |
| `--yes` | | no questions, defaults for everything not given |
| `--local DIR` | | take `vmlinuz` and `installer.img` from DIR instead of downloading them |
| `--repo URL` | synodeb repository | apt repository for the installed system |
| `--replace-debian` | | when the DOM has no room for the installer next to the kernel of an installed Debian, remove that Debian from the DOM instead of stopping (without it the script asks, even with `--yes`) |
| `--experimental` | | allow a model whose support is work in progress |

Every write to the DOM works on a copy: the copy is checked, the partition is checked to be
unchanged since it was read, then the copy is written back with `dd`, read back and compared.
If a write fails, what was there before is written back automatically. While the DOM is
written, a dropped SSH session, Ctrl-C and `kill` (TERM) are ignored.

The one-time SSH password of the installer is only in the INSTALLER boot entry on the DOM (and
on the installer's kernel command line): it is removed when the installation completes, by
`cancel` and by `uninstall`. DOM backups are readable by root only.
