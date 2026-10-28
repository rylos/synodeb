#!/bin/bash
# tests/make-test-dom.sh OUT.img: a synthetic DS923+ DOM for tests/run-test-setup.sh, with no
# Synology files: MBR, p1 FAT (volume id 10EE-589C) with a GRUB2 SynoBootLoader.conf of the
# same shape as the real one, p2 ext2 (the DS923+ UUID) with dummy zImage and rd.gz.
# Runs in a privileged debian:trixie container (loop devices).
set -euo pipefail
OUT=$(realpath "$1")
touch "$OUT"
exec docker run --rm --privileged -v "$OUT":/out.img debian:trixie bash -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq fdisk e2fsprogs dosfstools util-linux >/dev/null
for i in $(seq 0 31); do [ -e /dev/loop$i ] || mknod -m 660 /dev/loop$i b 7 $i; done
I=/tmp/dom.img; M=/tmp/m; mkdir -p $M
truncate -s 128M $I
printf "label: dos\nstart=2048, size=34816, type=ef, bootable\nstart=36864, size=167936, type=83\n" | sfdisk -q $I
L1=$(losetup -f --show -o $((2048 * 512)) --sizelimit $((34816 * 512)) $I)
L2=$(losetup -f --show -o $((36864 * 512)) --sizelimit $((167936 * 512)) $I)
mkfs.vfat -i 10EE589C -n SYNOBOOT1 $L1 >/dev/null
mke2fs -q -t ext2 -U 45e5b07d-4783-4867-a369-f99c0cd1e610 -L SYNOBOOT2 $L2
mount $L1 $M; mkdir -p $M/EFI/boot
cat > $M/EFI/boot/SynoBootLoader.conf <<EOT
serial --port=0xfedc9000 --speed=115200
terminal_input serial
terminal_output serial
set default='"'"'1'"'"'
set timeout='"'"'3'"'"'
set fallback='"'"'0'"'"'

menuentry "SYNOLOGY_1" {
        search --fs-uuid --no-floppy --set=root 45e5b07d-4783-4867-a369-f99c0cd1e610
        linux /zImage root=/dev/md0 syno_hw_version=DS923+ console=uart8250,mmio32,0xfedc9000,115200n8
        initrd /rd.gz
}
menuentry "SYNOLOGY_2" {
        search --fs-uuid --no-floppy --set=root 45e5b07d-4783-4867-a369-f99c0cd1e610
        linux /zImage root=/dev/md0 syno_hw_version=DS923+ console=uart8250,mmio32,0xfedc9000,115200n8
        initrd /rd.gz
}
EOT
umount $M
mount $L2 $M; head -c 1M /dev/zero > $M/zImage; head -c 1M /dev/zero > $M/rd.gz; umount $M
losetup -d $L1 $L2
cp $I /out.img'
