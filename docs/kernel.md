# Kernel

The kernel is a kernel.org release with Debian's configuration of that branch and the synodeb
drivers for the supported models. One kernel package serves every model.

The packages are named after the first supported model (`linux-image-ds923*`,
`synology-ds923`): the names stay as they are, so that existing systems keep updating.

## Series

There are two, named as on [kernel.org](https://www.kernel.org/category/releases.html):

| metapackage | series | |
|---|---|---|
| `linux-image-ds923-longterm` | longterm (6.18.y), projected end of life December 2028 | **default**, also installed as `linux-image-ds923` |
| `linux-image-ds923-stable` | the newest stable branch (7.3.y, then 7.4.y…) | newer features, each branch maintained for a few months only |

To follow the stable series: `sudo apt install linux-image-ds923-stable` (plus
`linux-headers-ds923-stable` if you build modules). With both installed, the DOM boots the
newest stable kernel; remove `linux-image-ds923-stable` to go back to longterm.

## Updates

New releases of both series are built and published automatically, usually within a day of
their release on kernel.org. Install them with `sudo apt update && sudo apt full-upgrade`, then
reboot. A kernel update is never installed automatically, because it writes the DOM.

The DOM keeps two kernels: if the new one does not boot, the next boot starts the previous one
and you get a notification. A normal boot never writes the DOM. A kernel update writes about
25 MB to the DOM; Synology does not publish its endurance, but even MLC flash should last
thousands of updates.

## Headers and modules

For DKMS modules install `linux-headers-ds923` (or `linux-headers-ds923-stable`): it follows the
kernel of its series. Module signatures are checked but not enforced, so locally built modules
load. ZFS (`zfs-dkms`) works with the longterm series only, until OpenZFS supports the newest
stable branch.

## RAID arrays and kernel 7.x

RAID arrays created by a 7.x kernel record their logical block size in the md superblock, and a
stock 6.18 kernel refuses to assemble them. The synodeb longterm kernel carries a small patch
that reads and keeps that field, so these arrays work with both series. Moving such disks to
another machine needs a kernel newer than 6.18 there.
