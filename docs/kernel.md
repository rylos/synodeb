# Kernel

The kernel is a kernel.org release with Debian's configuration of that branch and the synodeb
drivers for the supported models. One kernel package serves every model.

Until November 2026 the packages were named after the first supported model:
`linux-image-ds923*` and `synology-ds923`, kernels `<version>-ds923`. Those names are now
transitional packages: `sudo apt full-upgrade` moves an existing system to the `synodeb` names
by itself, and the transitional packages can be removed afterwards.

## Series

There are two, named as on [kernel.org](https://www.kernel.org/category/releases.html):

| metapackage | series | |
|---|---|---|
| `linux-image-synodeb-longterm` | longterm (6.18.y), projected end of life December 2028 | **default**, also installed as `linux-image-synodeb` |
| `linux-image-synodeb-stable` | the newest stable branch (7.3.y, then 7.4.y…) | newer features, each branch maintained for a few months only |

To follow the stable series: `sudo apt install linux-image-synodeb-stable` (plus
`linux-headers-synodeb-stable` if you build modules). With both installed, the DOM boots the
newest stable kernel; remove `linux-image-synodeb-stable` to go back to longterm.

## Updates

New releases of both series are built and published automatically, usually within a day of
their release on kernel.org. Install them with `sudo apt update && sudo apt full-upgrade`, then
reboot. A kernel update is never installed automatically, because it writes the DOM.

The DOM keeps two kernels: if the new one does not boot, the next boot starts the previous one
and you get a notification. A normal boot never writes the DOM. A kernel update writes about
25 MB to the DOM; Synology does not publish its endurance, but even MLC flash should last
thousands of updates.

## Headers and modules

For DKMS modules install `linux-headers-synodeb` (or `linux-headers-synodeb-stable`): it follows the
kernel of its series. Module signatures are checked but not enforced, so locally built modules
load. ZFS (`zfs-dkms`) works with the longterm series only, until OpenZFS supports the newest
stable branch.

## RAID arrays and kernel 7.x

RAID arrays created by a 7.x kernel record their logical block size in the md superblock, and a
stock 6.18 kernel refuses to assemble them. The synodeb longterm kernel carries a small patch
that reads and keeps that field, so these arrays work with both series. Moving such disks to
another machine needs a kernel newer than 6.18 there.
