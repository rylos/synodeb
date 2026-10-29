#!/bin/bash
# Replaces /synodeb/installer.img and installer.vmlinuz in the test DOM image with build/out/installer
set -euo pipefail
apt-get update -qq && apt-get install -y -qq e2fsprogs fdisk >/dev/null
I=/dom.img
read -r s z < <(sfdisk -d $I 2>/dev/null | awk -F'[=,]' '/start=/{ if (++i == 2) { gsub(/ /, ""); print $2, $4 } }')
dd if=$I of=/tmp/p2 bs=512 skip=$s count=$z status=none
for f in installer.img:installer.img vmlinuz:installer.vmlinuz; do
    debugfs -w -R "rm /synodeb/${f#*:}" /tmp/p2 >/dev/null 2>&1 || true
    debugfs -w -R "write /work/build/out/installer/${f%:*} /synodeb/${f#*:}" /tmp/p2 >/dev/null
done
e2fsck -fn /tmp/p2 >/dev/null
dd if=/tmp/p2 of=$I bs=512 seek=$s conv=notrunc status=none
echo "installer replaced"
