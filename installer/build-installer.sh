#!/bin/bash
# Builds the synodeb installer: the official Debian trixie netboot installer (debian-installer)
# with the kernel modules of the synodeb kernel instead of Debian's, plus the synodeb preseed
# and scripts. Runs in a debian:trixie container (see run-build.sh).
# Output in build/out/installer: vmlinuz (synodeb kernel), installer.img (initrd), SHA256SUMS.
set -euo pipefail
W=/work
OUT=$W/build/out
CACHE=$W/build/installer-cache
DEST=$OUT/installer
MIRROR=${MIRROR:-https://deb.debian.org/debian}
DIST=trixie
# the installer runs the kernel of the default series (longterm)
# shellcheck source=/dev/null
. $W/kernel/longterm/VERSION
KDEB=$OUT/linux-image-${KREL}_${KPKGVER}_amd64.deb
[ -f "$KDEB" ] || { echo "$KDEB missing: build the kernel first (kernel/run-build.sh)" >&2; exit 1; }

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq curl gpgv debian-archive-keyring cpio xz-utils kmod dpkg-dev python3 >/dev/null
mkdir -p "$CACHE" "$DEST"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# --- downloads, verified against the signed InRelease of the Debian archive ---
curl -fsSL -o "$T/InRelease" "$MIRROR/dists/$DIST/InRelease"
gpgv --keyring /usr/share/keyrings/debian-archive-keyring.gpg --output "$T/Release" "$T/InRelease" 2>/dev/null ||
    { echo "InRelease signature not valid" >&2; exit 1; }
release_sha() { awk -v f="$1" '/^SHA256:/ { s = 1; next } /^[^ ]/ { s = 0 } s && $3 == f { print $1 }' "$T/Release"; }
fetch() { # url file sha256
    if [ ! -f "$2" ] || [ "$(sha256sum "$2" | cut -d' ' -f1)" != "$3" ]; then
        curl -fsSL -o "$2.tmp" "$1"
        mv "$2.tmp" "$2"
    fi
    [ "$(sha256sum "$2" | cut -d' ' -f1)" = "$3" ] || { echo "wrong sha256: $1" >&2; exit 1; }
}
IMG=main/installer-amd64/current/images
fetch "$MIRROR/dists/$DIST/$IMG/SHA256SUMS" "$T/SHA256SUMS" "$(release_sha $IMG/SHA256SUMS)"
want=$(awk '$2 == "./netboot/debian-installer/amd64/initrd.gz" { print $1 }' "$T/SHA256SUMS")
fetch "$MIRROR/dists/$DIST/$IMG/netboot/debian-installer/amd64/initrd.gz" "$CACHE/initrd.gz" "$want"
PK=main/debian-installer/binary-amd64/Packages.xz
fetch "$MIRROR/dists/$DIST/$PK" "$CACHE/udeb-Packages.xz" "$(release_sha $PK)"
xz -dc "$CACHE/udeb-Packages.xz" > "$T/Packages"

# --- the netboot initrd ---
R=$T/root
mkdir -p "$R"
(cd "$R" && zcat "$CACHE/initrd.gz" | cpio -id --quiet)
KV=$(ls "$R/lib/modules")
[ "$(echo "$KV" | wc -l)" = 1 ] || { echo "unexpected lib/modules in the initrd: $KV" >&2; exit 1; }
echo "debian-installer kernel $KV -> $KREL"

# --- synodeb kernel ---
mkdir -p "$T/k"
dpkg-deb -x "$KDEB" "$T/k"
KM=$T/k/lib/modules/$KREL
[ -d "$KM" ] || KM=$T/k/usr/lib/modules/$KREL
[ -d "$KM" ] || { echo "no modules for $KREL in $KDEB" >&2; exit 1; }
KPKGVER=$(dpkg-deb -f "$KDEB" Version)

# --- module udebs: same groups as Debian, with the synodeb modules ---
# Groups the DS923+ cannot use are left out.
SKIP='^(sound|speakup|pcmcia|pcmcia-storage|nic-pcmcia|nic-wireless|firewire-core|mmc|mmc-core|pata|rfkill|ppp)-modules-'
udebs=$(awk -v kv="$KV" '/^Package:/ { p = $2 } /^Kernel-Version:/ && $2 == kv { print p }' "$T/Packages")
NM=$R/lib/modules/$KREL
mkdir -p "$NM"
declare -A wanted
status_add=$T/status-add
: > "$status_add"
for u in $udebs; do
    info=$(awk -v p="$u" '/^Package:/ { s = ($2 == p) } s' "$T/Packages")
    provides=$(echo "$info" | sed -n 's/^Provides: //p')
    newname=${u/$KV/$KREL}
    if [[ ! $u =~ ^kernel-image- ]] && [[ $u =~ $SKIP ]]; then
        continue
    fi
    file=$(echo "$info" | sed -n 's/^Filename: //p')
    sha=$(echo "$info" | sed -n 's/^SHA256: //p')
    fetch "$MIRROR/$file" "$CACHE/$(basename "$file")" "$sha"
    n=0
    while read -r ko; do
        rel=${ko#*/lib/modules/$KV/}
        base=${rel%.ko*}
        for c in "$KM/$base.ko" "$KM/$base.ko.xz" "$KM/$base.ko.zst"; do
            [ -f "$c" ] && { wanted[${c#"$KM"/}]=1; n=$((n + 1)); break; }
        done
    done < <(dpkg-deb -c "$CACHE/$(basename "$file")" | awk '{ print $6 }' | grep '\.ko')
    printf 'Package: %s\nStatus: install ok unpacked\nVersion: %s\n%sDescription: %s (synodeb kernel %s)\n\n' \
        "$newname" "$KPKGVER" "${provides:+Provides: $provides
}" "$(echo "$info" | sed -n 's/^Description: //p' | head -1)" "$KREL" >> "$status_add"
    echo "  $newname: $n modules"
done
# modules the installer scripts need beyond Debian's groups: FAT code pages for the DOM
for m in fs/nls/nls_cp437 fs/nls/nls_ascii fs/nls/nls_utf8; do
    for c in "$KM/kernel/$m.ko" "$KM/kernel/$m.ko.xz"; do [ -f "$c" ] && wanted[${c#"$KM"/}]=1; done
done
# dependencies (modules.dep of the synodeb kernel)
changed=1
while [ $changed = 1 ]; do
    changed=0
    for m in "${!wanted[@]}"; do
        for d in $(awk -v m="$m:" '$1 == m { $1 = ""; print }' "$KM/modules.dep"); do
            [ -n "${wanted[$d]:-}" ] || { wanted[$d]=1; changed=1; }
        done
    done
done
for m in "${!wanted[@]}"; do
    install -D -m 644 "$KM/$m" "$NM/$m"
done
cp "$KM"/modules.builtin* "$KM"/modules.order "$NM"/
depmod -b "$R" -F "$T/k/boot/System.map-$KREL" "$KREL"
echo "${#wanted[@]} modules for $KREL"

# replace Debian's kernel and module udebs in the installer's package database
rm -rf "$R/lib/modules/$KV"
awk -v kv="-$KV-di" 'BEGIN { RS = ""; ORS = "\n\n" } { split($0, l, "\n"); p = l[1]; sub(/^Package: /, "", p) }
     index(p, kv) == 0 { print }' "$R/var/lib/dpkg/status" > "$T/status"
cat "$status_add" >> "$T/status"
cp "$T/status" "$R/var/lib/dpkg/status"

# --- reconnecting over SSH ---
# lib/debian-installer.d/S70menu resumes a running GNU screen only if it finds it in
# /var/run/screen, but screen keeps its sockets in /run/screen (and /var/run is not a link to
# /run in the installer): every SSH login started a second installer. Here a login attaches
# to the session of an earlier SSH login, if there is one, so a dropped connection resumes
# the installation where it was.
M=$R/lib/debian-installer.d/S70menu
old='		if [ -d /var/run/screen/S-root -a -e /var/run/screen/S-root/* ]
		then
			$screen_bin $SCREEN_OPT -x -p 1	# If GNU/screen is already started, just resume it'
new='		s=$(ls /run/screen/S-root 2>/dev/null | grep "\.pts-" | head -n 1)	# synodeb
		if [ -n "$s" ]
		then
			$screen_bin $SCREEN_OPT -x "$s" -p 1	# resume the installation of an earlier SSH login'
python3 - "$M" "$old" "$new" <<'PY'
import sys
f, old, new = sys.argv[1:]
t = open(f).read()
if old not in t:
    sys.exit("S70menu: unexpected content, cannot patch the SSH reconnect")
open(f, "w").write(t.replace(old, new))
PY

# --- synodeb preseed and scripts ---
S=$R/usr/lib/synodeb-installer
mkdir -p "$S"
cp $W/installer/files/preseed.cfg "$R/preseed.cfg"
install -m 644 $W/installer/files/lib.sh "$S/"
install -m 755 $W/installer/files/early $W/installer/files/finish "$S/"
install -m 644 $W/pkg/synology-ds923/etc/apt/sources.list.d/synodeb.sources \
    $W/pkg/synology-ds923/etc/apt/preferences.d/synodeb-no-debian-kernel \
    $W/pkg/synology-ds923/usr/share/keyrings/synodeb-archive-keyring.gpg "$S/"

# --- output ---
rm -f "$DEST"/*
(cd "$R" && find . | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet) | xz --check=crc32 -9e -T0 > "$DEST/installer.img"
cp "$T/k/boot/vmlinuz-$KREL" "$DEST/vmlinuz"
echo "$KREL $KPKGVER, debian-installer $DIST ($KV)" > "$DEST/VERSION"
(cd "$DEST" && sha256sum vmlinuz installer.img VERSION > SHA256SUMS)
ls -la "$DEST"
