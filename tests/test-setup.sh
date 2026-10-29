#!/bin/bash
# Runs synodeb-setup.sh against a copy of a DOM image (loop devices), in a privileged
# debian:trixie container; see run-test-setup.sh. The image must be a DOM without synodeb
# (as DSM leaves it). Exits 1 if any check fails.
#   OUT=1      saves the DOM right after 'install' to /out.img (for the VM test)
#   CAPTURE=1  writes terminal captures for the README to /captures
#   NET, REPO  --network and --repo of the first install
#   DOWNLOAD=1 the first install downloads the installer (published release) instead of --local
set -euo pipefail
W=/work
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq fdisk e2fsprogs dosfstools iproute2 util-linux curl ca-certificates >/dev/null
for i in $(seq 0 31); do [ -e /dev/loop$i ] || mknod -m 660 /dev/loop$i b 7 $i; done
T=/tmp/dom
mkdir -p $T/bk $T/b
cp /dom.img $T/dom.img
part() { sfdisk -d $T/dom.img 2>/dev/null | awk -F'[=,]' -v n="$1" '/start=/{ if (++i == n) { gsub(/ /, ""); print $2, $4 } }'; }
read -r s1 z1 < <(part 1)
read -r s2 z2 < <(part 2)
SYNOBOOT=$(losetup -f --show $T/dom.img)
SYNOBOOT1=$(losetup -f --show -o $((s1 * 512)) --sizelimit $((z1 * 512)) $T/dom.img)
SYNOBOOT2=$(losetup -f --show -o $((s2 * 512)) --sizelimit $((z2 * 512)) $T/dom.img)
export SYNOBOOT SYNOBOOT1 SYNOBOOT2 SYNODEB_TEST=1
trap 'umount $T/b 2>/dev/null || true; losetup -d $SYNOBOOT $SYNOBOOT1 $SYNOBOOT2' EXIT
S="bash $W/synodeb-setup.sh"
INST=$W/build/out/installer
LOCAL=(--local "$INST")
[ -z "${DOWNLOAD:-}" ] || LOCAL=()
cd $T/bk

FAILS=0
check() { # description shell-command
    if eval "$2"; then echo "== OK: $1"; else echo "== FAIL: $1"; FAILS=$((FAILS + 1)); fi
}
conf_sha() { # sha256 of SynoBootLoader.conf in a FAT image
    mount -o loop,ro "$1" $T/b && sha256sum $T/b/EFI/boot/SynoBootLoader.conf | cut -c1-64; umount $T/b
}
dom_sha() { sha256sum < "$SYNOBOOT" | cut -c1-64; }
st() { # status in $T/st, SynoBootLoader.conf in $T/conf
    $S status > $T/st 2>&1 &&
    mount -o ro "$SYNOBOOT1" $T/b && cp $T/b/EFI/boot/SynoBootLoader.conf $T/conf && umount $T/b
}
# a fake installed Debian: DEBIAN/DEBIAN_OLD entries (as synodeb-dom setup writes them) and
# kernel files of the given sizes (MB) on the DOM
fake_debian() { # vmlinuz-MB initrd-MB [old-vmlinuz-MB old-initrd-MB]
    mount "$SYNOBOOT2" $T/b
    mkdir -p $T/b/synodeb
    head -c "$(($1 << 20))" /dev/urandom > $T/b/synodeb/vmlinuz
    head -c "$(($2 << 20))" /dev/urandom > $T/b/synodeb/initrd.img
    echo test > $T/b/synodeb/VERSION
    if [ -n "${3:-}" ]; then
        head -c "$(($3 << 20))" /dev/urandom > $T/b/synodeb/vmlinuz.old
        head -c "$(($4 << 20))" /dev/urandom > $T/b/synodeb/initrd.img.old
    fi
    umount $T/b
    mount "$SYNOBOOT1" $T/b
    c=$T/b/EFI/boot/SynoBootLoader.conf
    [ -f $c.dsm ] || cp $c $c.dsm
    { cat $T/b/EFI/boot/SynoBootLoader.conf.dsm
      echo '### synodeb begin (written by synodeb-dom setup; settings in /etc/default/synodeb)'
      for e in DEBIAN:vmlinuz:initrd.img DEBIAN_OLD:vmlinuz.old:initrd.img.old; do
          IFS=: read -r t k i <<< "$e"
          printf 'menuentry "%s" {\n        search --fs-uuid --no-floppy --set=root 45e5b07d-4783-4867-a369-f99c0cd1e610\n        linux /synodeb/%s root=UUID=1234 ro synodeb.entry=%s\n        initrd /synodeb/%s\n}\n' "$t" "$k" "$t" "$i"
      done
      echo '### synodeb end'; } | sed "s/^set default='[0-9]*'/set default='2'/" > $c.new
    mv $c.new $c
    umount $T/b
}
has() { echo " $(debugfs -R 'ls /synodeb' "$SYNOBOOT2" 2>/dev/null | tr -s ' \n' '  ') " | grep -q " $1 "; }

ORIG=$(dom_sha)
$S status
# --- fresh install, switching, cancel
check install "$S install --yes --network '${NET:-dhcp}' --hostname nas --lang en ${LOCAL[*]+${LOCAL[*]}} ${REPO:+--repo '$REPO'} | tee $T/install.out && grep -q 'Password:' $T/install.out"
B0=$(sed -n 's/^DOM backup: *//p' $T/install.out)
check "backup of the original DOM" "[ -n '$B0' ] && [ \"\$(sha256sum < '$B0' | cut -c1-64)\" = $ORIG ]"
check "backup readable by root only" "[ \"\$(stat -c %a '$B0')\" = 600 ]"
check "INSTALLER default" "st && grep -q 'INSTALLER   <- default' $T/st"
check "installer kernel as installer.vmlinuz" "has installer.vmlinuz && ! has vmlinuz"
if [ -n "${OUT:-}" ]; then cp $T/dom.img /out.img; echo "== DOM saved"; fi
check "boot dsm" "$S boot dsm && st && grep -q 'SYNOLOGY_2   <- default' $T/st"
check "boot debian refused without Debian" "! $S boot debian 2>/dev/null"
check cancel "$S cancel && st && ! grep -q -e INSTALLER -e 'synodeb:' $T/st"

# --- options and their checks
before=$(dom_sha)
check "options before the command" "$S --yes --network dsm ${LOCAL[*]+${LOCAL[*]}} install > /dev/null && st && grep -q 'INSTALLER   <- default' $T/st"
check "static network on the command line" "grep -q 'netcfg/get_ipaddress=[0-9]' $T/conf"
$S cancel >/dev/null
# DSM on DHCP: the installer defaults to DHCP too
dev=$(ip -4 route show default | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
mkdir -p /etc/sysconfig/network-scripts
echo 'BOOTPROTO=dhcp' > "/etc/sysconfig/network-scripts/ifcfg-$dev"
check "DSM on DHCP: installer on DHCP" "$S install --yes ${LOCAL[*]+${LOCAL[*]}} > $T/i1.out && st && grep -q 'from DHCP' $T/i1.out && grep -q 'choose_interface=' $T/conf && ! grep -q 'get_ipaddress=' $T/conf"
rm -rf /etc/sysconfig
$S cancel >/dev/null
before=$(dom_sha)
check "bad host name refused" "! $S install --yes --hostname 'a b' ${LOCAL[*]+${LOCAL[*]}} 2>/dev/null"
check "host name with a leading - refused" "! $S install --yes --hostname -x ${LOCAL[*]+${LOCAL[*]}} 2>/dev/null"
check "bad repo refused" "! $S install --yes --repo 'https://x/\$(id)' ${LOCAL[*]+${LOCAL[*]}} 2>/dev/null"
check "repo without http refused" "! $S install --yes --repo ftp://x/ ${LOCAL[*]+${LOCAL[*]}} 2>/dev/null"
check "missing option value refused" "! $S install --yes --backup-dir 2>/dev/null"
check "unknown argument refused" "! $S status extra 2>/dev/null"
check "DOM unchanged by refused commands" "[ \"\$(dom_sha)\" = $before ]"

# --- an installed Debian stays when the installer is added again
fake_debian 12 11 12 11
check "boot debian with Debian" "$S boot debian >/dev/null && st && grep -q 'DEBIAN   <- default' $T/st"
check "install next to Debian" "$S install --yes --network dhcp ${LOCAL[*]+${LOCAL[*]}} > $T/i2.out"
check "DEBIAN_OLD files dropped for room" "grep -q 'DEBIAN_OLD) is removed' $T/i2.out && ! has vmlinuz.old"
check "Debian entries and kernel kept" "st && grep -q ' DEBIAN$' $T/st && grep -q 'INSTALLER   <- default' $T/st && has vmlinuz && has initrd.img"
check "cancel keeps Debian" "$S cancel >/dev/null && st && grep -q ' DEBIAN$' $T/st && grep -q 'SYNOLOGY_2   <- default' $T/st && ! grep -q INSTALLER $T/st && ! has installer.img && ! has installer.vmlinuz && has vmlinuz"
check "boot debian after cancel" "$S boot debian >/dev/null"
# no room for the installer next to Debian: refused without --replace-debian
$S uninstall --yes >/dev/null
fake_debian 30 20
before=$(dom_sha)
check "no room next to Debian: refused" "! $S install --yes ${LOCAL[*]+${LOCAL[*]}} > $T/i3.out 2>&1 && grep -q 'replace-debian' $T/i3.out && [ \"\$(dom_sha)\" = $before ]"
check "--replace-debian" "$S install --yes --replace-debian ${LOCAL[*]+${LOCAL[*]}} >/dev/null && st && ! grep -q DEBIAN $T/st && grep -q 'INSTALLER   <- default' $T/st"

# --- uninstall, restore
check uninstall "$S uninstall --yes >/dev/null"
dd if="$SYNOBOOT1" of=$T/p1.img bs=64k 2>/dev/null
dd if=/dom.img of=$T/o1.img bs=512 skip="$s1" count="$z1" 2>/dev/null
check "uninstall gives the original conf" "[ \"\$(conf_sha $T/p1.img)\" = \"\$(conf_sha $T/o1.img)\" ]"
head -c 1048576 "$B0" > $T/short.img
check "restore refuses an image of another size" "! $S restore --yes $T/short.img 2>/dev/null"
head -c "$(stat -c %s "$B0")" /dev/zero > $T/zero.img
check "restore refuses an image that is not a DOM" "! $S restore --yes $T/zero.img 2>/dev/null"
check "restore = original image" "$S restore '$B0' --yes && [ \"\$(dom_sha)\" = $ORIG ]"

# --- menu: stays after a failed action
check "menu stays after a failed action" "printf '2\n4\nq\n' | script -qc 'bash $W/synodeb-setup.sh' /dev/null > $T/menu.out 2>&1; grep -q 'not completed' $T/menu.out && grep -q 'Boot entries on the DOM' $T/menu.out"

# terminal captures for the README
if [ -n "${CAPTURE:-}" ]; then
    printf '4\nq\n' | script -qc "bash $W/synodeb-setup.sh" /dev/null > /captures/menu.txt 2>&1 || true
    printf '\n\nen\nyes\n' | script -qc "bash $W/synodeb-setup.sh install --hostname nas --local $INST" /dev/null > /captures/install.txt 2>&1
    $S status > /captures/status.txt 2>&1
    bash $W/synodeb-setup.sh --help > /captures/help.txt 2>&1
fi

if [ $FAILS != 0 ]; then echo "== $FAILS check(s) FAILED"; exit 1; fi
echo "== all checks passed"
