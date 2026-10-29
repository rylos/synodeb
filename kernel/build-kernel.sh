#!/bin/bash
# Builds vanilla Linux for one kernel series (SERIES=longterm|stable, kernel/<series>/VERSION)
# with the Debian configuration of its branch + the DS923+ fragment, and produces the .deb
# packages. Runs in a debian:trixie container (see run-build.sh).
set -euo pipefail
SERIES=${SERIES:-longterm}
[ -f "/work/kernel/$SERIES/VERSION" ] || { echo "unknown kernel series: $SERIES" >&2; exit 1; }
# shellcheck source=/dev/null
. "/work/kernel/$SERIES/VERSION"
PATCHES=/work/kernel/patches/$KBRANCH
CONFIG=/work/kernel/config-debian-$KBRANCH
[ -d "$PATCHES" ] && [ -f "$CONFIG" ] || { echo "$PATCHES or $CONFIG missing for $KVER" >&2; exit 1; }
VER=$KVER
SRC=/work/build/linux-$VER
OUT=/work/build/out
PKGVER=$KPKGVER
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential bc bison flex libssl-dev libelf-dev \
    dwarves libdw-dev rsync kmod cpio debhelper-compat python3 xz-utils zstd git lsb-release \
    curl gpg gpgv >/dev/null
mkdir -p "$OUT"
# kernel.org tarball, verified with the signed sha256sums.asc (keys of Greg Kroah-Hartman,
# Linus Torvalds and the kernel.org checksum autosigner: kernel/keys/kernel.org.asc)
TB=/work/build/linux-$VER.tar.xz
if [ ! -f "$TB" ]; then
    curl -fsSL -o "$TB.tmp" "https://cdn.kernel.org/pub/linux/kernel/v${VER%%.*}.x/linux-$VER.tar.xz"
    mv "$TB.tmp" "$TB"
fi
curl -fsSL -o /work/build/sha256sums.asc "https://cdn.kernel.org/pub/linux/kernel/v${VER%%.*}.x/sha256sums.asc"
gpg --batch -q --dearmor -o /tmp/kernel.org.gpg < /work/kernel/keys/kernel.org.asc
gpgv --keyring /tmp/kernel.org.gpg /work/build/sha256sums.asc 2>/dev/null || { echo "sha256sums.asc: bad signature" >&2; exit 1; }
want=$(awk -v f="linux-$VER.tar.xz" '$2 == f { print $1 }' /work/build/sha256sums.asc)
[ -n "$want" ] && [ "$(sha256sum "$TB" | cut -d' ' -f1)" = "$want" ] || { echo "$TB: wrong sha256" >&2; exit 1; }
# Sources: a linux-$VER tree under git (commit "vanilla" = the tarball). The patches
# (kernel/patches/<x.y>) are applied again only when they change, after resetting the files
# they touch to vanilla; the new drivers (kernel/src) are copied only when different.
# So make rebuilds only what changed. FULL=1 starts again from the tarball.
export GIT_AUTHOR_NAME=synodeb GIT_AUTHOR_EMAIL=synodeb@localhost
export GIT_COMMITTER_NAME=synodeb GIT_COMMITTER_EMAIL=synodeb@localhost
git config --global --add safe.directory "$SRC"
if [ "${FULL:-0}" = 1 ] || [ ! -d "$SRC/.git" ]; then
    rm -rf "$SRC"
    tar -C /work/build -xf /work/build/linux-$VER.tar.xz
    git -C "$SRC" init -q
    git -C "$SRC" add -A
    git -C "$SRC" commit -qm vanilla
fi
cd "$SRC"
STAMP=$(cat "$PATCHES"/*.patch | sha256sum | cut -d' ' -f1)
if [ "$(cat .synodeb-patches 2>/dev/null)" != "$STAMP" ]; then
    echo "patches changed: touched files reset to vanilla"
    # no stamp until every patch is in: a failed run must not look applied next time
    rm -f .synodeb-patches
    git checkout -q -- .
    # strict: no fuzz and no offsets, a patch that does not apply exactly must be refreshed
    for p in "$PATCHES"/*.patch; do
        echo "patch $(basename "$p")"
        out=$(patch -p1 --forward --fuzz=0 --no-backup-if-mismatch < "$p") ||
            { echo "$out"; echo "$p does not apply" >&2; exit 1; }
        echo "$out"
        if grep -q '^Hunk #[0-9]* succeeded' <<<"$out"; then
            echo "$p applies only with an offset: refresh it" >&2
            exit 1
        fi
    done
    echo "$STAMP" > .synodeb-patches
else
    echo "patches unchanged"
fi
rsync -rc --itemize-changes /work/kernel/src/ ./
cp "$CONFIG" .config
scripts/kconfig/merge_config.sh -m .config /work/kernel/ds923.config /work/kernel/ds923-slim.config
make olddefconfig
for o in SYNOLOGY_DS923=y LEDS_ATMEGA1608=m RTC_DRV_S35390A=y RTC_DRV_CMOS=m PINCTRL_AMD=y \
         SERIAL_8250_DW=y I2C_DESIGNWARE_PLATFORM=y EFI_VARS_PSTORE_DEFAULT_DISABLE=y \
         ATA_LEDS=y LEDS_TRIGGER_TIMER DEBUG_INFO_NONE; do
    grep -E "^CONFIG_$o" .config || { echo "missing CONFIG_$o"; exit 1; }
done
make -j"$(nproc)" bindeb-pkg KDEB_PKGVERSION="$PKGVER" LOCALVERSION="-$ABI-ds923"
[ "$(make -s kernelrelease LOCALVERSION="-$ABI-ds923")" = "$KREL" ] || { echo "kernelrelease is not $KREL" >&2; exit 1; }
rm -f ../linux-image-*-dbg_*.deb
mv ../linux-*.deb "$OUT"/
cp .config "$OUT"/config-$KREL
# Metapackages: always depend on the latest kernel of the series (like linux-image-amd64 in
# Debian). linux-image-ds923 is the default and follows the longterm series; its epoch keeps
# it newer than the 7.3.0-1 it had before the series were split.
meta() { # package version depends description
    d=/tmp/meta-$1
    rm -rf "$d"; mkdir -p "$d/DEBIAN"
    cat > "$d/DEBIAN/control" <<EOT
Package: $1
Version: $2
Architecture: amd64
Maintainer: Marco Ziliani <508111+rylos@users.noreply.github.com>
Depends: $3
Section: kernel
Priority: optional
Description: $4
EOT
    # the series decides which kernel the DOM boots (/usr/lib/synodeb/boot-kernels): installing
    # or removing linux-image-ds923-<series> alone must choose again
    case $1 in linux-image-ds923-*)
        for s in postinst postrm; do
            cat > "$d/DEBIAN/$s" <<'EOT'
#!/bin/sh
set -e
case "$1" in configure|remove|purge)
    [ ! -x /etc/kernel/postinst.d/zz-synodeb-boot-links ] || /etc/kernel/postinst.d/zz-synodeb-boot-links ;;
esac
EOT
            chmod 755 "$d/DEBIAN/$s"
        done ;;
    esac
    dpkg-deb --root-owner-group -Zxz -b "$d" "$OUT/${1}_${2#*:}_amd64.deb" >/dev/null
}
for kind in image headers; do
    meta "linux-$kind-ds923-$SERIES" "$PKGVER" "linux-$kind-$KREL (= $PKGVER)" \
        "Linux kernel for the Synology DS923+ ($kind, $SERIES series)
 Depends on the latest synodeb kernel $kind package of the $SERIES series ($KBRANCH)."
    if [ "$SERIES" = longterm ]; then
        meta "linux-$kind-ds923" "1:$PKGVER" "linux-$kind-ds923-longterm (= $PKGVER)" \
            "Linux kernel for the Synology DS923+ ($kind, default)
 Depends on the synodeb kernel of the longterm series. For the newest stable series
 install linux-$kind-ds923-stable instead."
    fi
done
ls -la "$OUT"
