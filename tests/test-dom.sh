#!/bin/bash
# tests/test-dom.sh DOM.img: synodeb-dom (sync, setup, default, kernel trial, status) against a
# copy of a DOM image made by make-test-dom.sh (GRUB 2 + ext2, or MODEL=DS415+: GRUB legacy +
# FAT), after synodeb-setup.sh has left it as DSM does. Runs in a privileged debian:trixie
# container (loop devices). Exits 1 if any check fails.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DOM=$(realpath "$1")
exec docker run --rm --privileged -v "$ROOT":/work:ro -v "$DOM":/dom.img:ro debian:trixie bash -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq fdisk dosfstools e2fsprogs util-linux python3 >/dev/null
for i in $(seq 0 31); do [ -e /dev/loop$i ] || mknod -m 660 /dev/loop$i b 7 $i; done
cp /dom.img /tmp/dom.img
part() { sfdisk -d /tmp/dom.img 2>/dev/null | awk -F"[=,]" -v n="$1" "/start=/{ if (++i == n) { gsub(/ /, \"\"); print \$2, \$4 } }"; }
read -r s1 z1 < <(part 1); read -r s2 z2 < <(part 2)
export SYNODEB_TEST_DOM1=$(losetup -f --show -o $((s1 * 512)) --sizelimit $((z1 * 512)) /tmp/dom.img)
export SYNODEB_TEST_DOM2=$(losetup -f --show -o $((s2 * 512)) --sizelimit $((z2 * 512)) /tmp/dom.img)
D="sh /work/pkg/synodeb/usr/sbin/synodeb-dom"
M=/tmp/m; mkdir -p $M
FAILS=0
check() { if eval "$2"; then echo "== OK: $1"; else echo "== FAIL: $1"; FAILS=$((FAILS + 1)); fi; }
conf() { mount -o ro $SYNODEB_TEST_DOM1 $M && cp $M/EFI/boot/SynoBootLoader.conf /tmp/conf && { [ ! -f $M/EFI/boot/synodeb-trial ] || cat $M/EFI/boot/synodeb-trial; } > /tmp/trial; umount $M; }
st() { $D status > /tmp/st 2>&1; grep -q "default entry: $1 ($2)" /tmp/st || { cat /tmp/st; false; }; }
p2() { mount -o ro $SYNODEB_TEST_DOM2 $M && rm -rf /tmp/l && mkdir /tmp/l && for f in vmlinuz vmlinuz.old initrd.img initrd.img.old VERSION VERSION.old; do [ ! -f $M/synodeb/$f ] || cp $M/synodeb/$f /tmp/l/; done; FS=$(findmnt -no FSTYPE $M); HL=$(stat -c %h $M/synodeb/vmlinuz 2>/dev/null); umount $M; }
kernel() { head -c 4M /dev/urandom > /boot/vmlinuz-$1; head -c 8M /dev/urandom > /boot/initrd.img-$1; }
mkdir -p /boot /etc/default
echo "UUID=1234 / ext4 defaults 0 1" > /etc/fstab
kernel 6.18.1-1-synodeb; kernel 6.18.2-2-synodeb
ln -sf vmlinuz-6.18.2-2-synodeb /boot/vmlinuz; ln -sf vmlinuz-6.18.1-1-synodeb /boot/vmlinuz.old
conf; LEGACY=0; grep -q "^default[[:space:]]" /tmp/conf && LEGACY=1; echo "== GRUB legacy: $LEGACY"
check "first sync" "$D sync"
p2
check "DEBIAN and DEBIAN_OLD on synoboot2 ($FS)" "cmp -s /tmp/l/vmlinuz /boot/vmlinuz-6.18.2-2-synodeb && cmp -s /tmp/l/vmlinuz.old /boot/vmlinuz-6.18.1-1-synodeb && grep -q 6.18.2-2 /tmp/l/VERSION"
check setup "$D setup"
conf
if [ $LEGACY = 1 ]; then
    check "GRUB legacy entries" "grep -qx \"title DEBIAN\" /tmp/conf && grep -qx \"title DEBIAN_OLD\" /tmp/conf && grep -q \"^	kernel /synodeb/vmlinuz root=UUID=1234 ro .* synodeb.entry=DEBIAN\$\" /tmp/conf && grep -qx \"	initrd /synodeb/initrd.img.old\" /tmp/conf && grep -qx \"	vender /vender show\" /tmp/conf && ! grep -q -e menuentry -e \"set default\" /tmp/conf"
    check "default DEBIAN (2), fallback DEBIAN_OLD (3)" "grep -qx \"default 2\" /tmp/conf && grep -qx \"fallback 3\" /tmp/conf"
else
    check "GRUB 2 entries" "grep -q \"^menuentry \\\"DEBIAN\\\"\" /tmp/conf && grep -q \"^set default=.2.\" /tmp/conf && grep -q \"^set fallback=.3 1 0.\" /tmp/conf"
fi
check "status" "st DEBIAN 2"
check "status --json" "$D status --json | python3 -c \"import json,sys; d=json.load(sys.stdin); assert d[\\\"default\\\"]==\\\"DEBIAN\\\" and d[\\\"entries\\\"][-2:]==[\\\"DEBIAN\\\",\\\"DEBIAN_OLD\\\"] and d[\\\"fallback\\\"][0]==\\\"DEBIAN_OLD\\\" and d[\\\"kernels\\\"][\\\"DEBIAN\\\"][\\\"release\\\"]==\\\"6.18.2-2-synodeb\\\" and d[\\\"outdated\\\"] is None, d\""
check "default dsm" "$D default dsm >/dev/null && conf && st SYNOLOGY_2 1"
check "default debian" "$D default debian >/dev/null && conf && st DEBIAN 2"
# kernel update: the running kernel stays as DEBIAN_OLD, the new one becomes DEBIAN on trial
kernel 6.18.3-3-synodeb
ln -sf vmlinuz-6.18.3-3-synodeb /boot/vmlinuz; ln -sf vmlinuz-6.18.2-2-synodeb /boot/vmlinuz.old
check "sync of a new kernel" "$D sync > /tmp/sync.out 2>&1"
p2; conf
check "new DEBIAN, previous as DEBIAN_OLD" "cmp -s /tmp/l/vmlinuz /boot/vmlinuz-6.18.3-3-synodeb && cmp -s /tmp/l/vmlinuz.old /boot/vmlinuz-6.18.2-2-synodeb && cmp -s /tmp/l/initrd.img.old /boot/initrd.img-6.18.2-2-synodeb"
check "kernel trial armed" "grep -qx armed /tmp/trial"
if [ "$FS" = vfat ]; then check "FAT: DEBIAN_OLD copied, not linked" "[ \"$HL\" = 1 ]"; fi
check "sync again: nothing to do" "$D sync > /tmp/sync2.out 2>&1 && ! grep -q \"new kernel\" /tmp/sync2.out"
check "default debian-old" "$D default debian-old >/dev/null && conf && st DEBIAN_OLD 3 && [ ! -s /tmp/trial ]"
if [ $LEGACY = 1 ]; then check "fallback DEBIAN while DEBIAN_OLD is the default" "grep -qx \"fallback 2\" /tmp/conf"; fi
check "Synology lines untouched" "sed -n \"/^### synodeb begin/q;p\" /tmp/conf | grep -v -e \"^default\" -e \"^fallback\" -e \"^set default\" -e \"^set fallback\" > /tmp/a; mount -o ro $SYNODEB_TEST_DOM1 $M; grep -v -e \"^default\" -e \"^fallback\" -e \"^set default\" -e \"^set fallback\" $M/EFI/boot/SynoBootLoader.conf.dsm > /tmp/b; umount $M; sed -i -e :a -e \"/^\\n*\$/{\\\$d;N;ba\" -e \"}\" /tmp/a /tmp/b; cmp -s /tmp/a /tmp/b"
losetup -d $SYNODEB_TEST_DOM1 $SYNODEB_TEST_DOM2
if [ $FAILS = 0 ]; then echo "== all checks passed"; else echo "== $FAILS checks failed"; exit 1; fi'
