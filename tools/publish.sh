#!/bin/bash
# Builds the GitHub Pages tree in build/pages (branch gh-pages):
#   debian/      signed apt repository (trixie main): kernels and metapackages of both series
#                (longterm, stable), synology-ds923
#   installer/<kernel release>/   vmlinuz, installer.img, SHA256SUMS, VERSION
#   synodeb-archive.asc           public key of the repository
# and writes the installer checksums into synodeb-setup.sh.
# SERIES=longterm (default) or stable: the kernel series whose packages in build/out are
# added; the other series stays as it is in the repository. The installer runs the longterm
# kernel, so it is published only with SERIES=longterm.
# Runs on the host; the repository is built in a debian:trixie container. The signing key is
# in $SYNODEB_GNUPG (default ~/.local/share/synodeb/gnupg), never in the project.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GNUPG=${SYNODEB_GNUPG:-$HOME/.local/share/synodeb/gnupg}
[ -d "$GNUPG" ] || { echo "signing key not found in $GNUPG" >&2; exit 1; }
# shellcheck source=/dev/null
SERIES=${SERIES:-longterm}
[ -f "$ROOT/kernel/$SERIES/VERSION" ] || { echo "unknown kernel series: $SERIES" >&2; exit 1; }
# shellcheck source=/dev/null
. "$ROOT/kernel/$SERIES/VERSION"
[ "$SERIES" = longterm ] || SKIP_INSTALLER=1
OUT=$ROOT/build/out
INST=$OUT/installer
# SKIP_INSTALLER=1: publish the packages only, the installer stays as it is
if [ "${SKIP_INSTALLER:-0}" != 1 ]; then
    [ -f "$INST/installer.img" ] || { echo "build the installer first (installer/run-build.sh)" >&2; exit 1; }
    grep -q "^$KREL " "$INST/VERSION" || { echo "installer built for another kernel: $(cat "$INST/VERSION")" >&2; exit 1; }
fi
PKGVER=$(sed -n 's/^Version: //p' "$ROOT/pkg/synology-ds923/DEBIAN/control")

docker run --rm -v "$ROOT":/work -v "$GNUPG":/gnupg -e KREL="$KREL" -e PKGVER="$PKGVER" \
    -e KDEBVER="$KPKGVER" -e SERIES="$SERIES" -e HOSTUID="$(id -u)" -e SKIP_INSTALLER="${SKIP_INSTALLER:-0}" debian:trixie bash -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq apt-utils gpg dpkg-dev >/dev/null
P=/work/build/pages
R=$P/debian
mkdir -p $R/pool/main $R/dists/trixie/main/binary-amd64 $P/installer
# synology-ds923
dpkg-deb --root-owner-group -Zxz -b /work/pkg/synology-ds923 /work/build/out/synology-ds923_${PKGVER}_all.deb >/dev/null
# metapackages of this series: only the new version (linux-image-ds923 has an epoch, so the
# file names do not sort by version)
metas="linux-image-ds923-$SERIES linux-headers-ds923-$SERIES"
[ "$SERIES" = longterm ] && metas="$metas linux-image-ds923 linux-headers-ds923"
for m in $metas; do
    rm -f $R/pool/main/${m}_*.deb
    cp /work/build/out/${m}_${KDEBVER}_amd64.deb $R/pool/main/
done
for f in linux-image-${KREL}_${KDEBVER}_amd64.deb linux-headers-${KREL}_${KDEBVER}_amd64.deb \
         synology-ds923_${PKGVER}_all.deb; do
    cp /work/build/out/$f $R/pool/main/
done
# keep the two most recent kernels of each branch (the previous one is DEBIAN_OLD on the DOM)
for b in $(ls $R/pool/main/linux-image-[0-9]*_amd64.deb | sed "s|.*/linux-image-\([0-9]*\.[0-9]*\)\..*|\1|" | sort -u); do
    ls $R/pool/main/linux-image-$b.*_amd64.deb | sort -V | head -n -2 | while read -r f; do
        v=$(basename "$f" | sed "s/^linux-image-\([^_]*\)_.*/\1/")
        rm -f $R/pool/main/linux-image-${v}_* $R/pool/main/linux-headers-${v}_*
    done
done
ls $R/pool/main/synology-ds923_*.deb | sort -V | head -n -2 | xargs -r rm -f
cd $R
apt-ftparchive packages pool > dists/trixie/main/binary-amd64/Packages
gzip -9kf dists/trixie/main/binary-amd64/Packages
xz -9kf dists/trixie/main/binary-amd64/Packages
apt-ftparchive -o APT::FTPArchive::Release::Origin=synodeb -o APT::FTPArchive::Release::Label=synodeb \
    -o APT::FTPArchive::Release::Suite=trixie -o APT::FTPArchive::Release::Codename=trixie \
    -o APT::FTPArchive::Release::Architectures=amd64 -o APT::FTPArchive::Release::Components=main \
    -o "APT::FTPArchive::Release::Description=synodeb: Debian on the Synology DS923+" \
    release dists/trixie > /tmp/Release
mv /tmp/Release dists/trixie/Release
export GNUPGHOME=/tmp/gnupg; cp -a /gnupg $GNUPGHOME; chown -R root:root $GNUPGHOME; chmod 700 $GNUPGHOME
gpg --batch -q --yes --clearsign -o dists/trixie/InRelease dists/trixie/Release
gpg --batch -q --yes -abs -o dists/trixie/Release.gpg dists/trixie/Release
gpg --batch --export --armor > $P/synodeb-archive.asc
# installer
if [ "$SKIP_INSTALLER" != 1 ]; then
rm -rf $P/installer/$KREL; mkdir -p $P/installer/$KREL
cp /work/build/out/installer/{vmlinuz,installer.img,SHA256SUMS,VERSION} $P/installer/$KREL/
ls $P/installer | sort -V | head -n -2 | while read -r d; do rm -rf "$P/installer/$d"; done
fi
touch $P/.nojekyll
chown -R $HOSTUID:$HOSTUID $P /work/build/out/synology-ds923_${PKGVER}_all.deb
'
[ "${SKIP_INSTALLER:-0}" != 1 ] || exit 0
# checksums of this installer in synodeb-setup.sh
sv=$(sha256sum "$INST/vmlinuz" | cut -d' ' -f1)
si=$(sha256sum "$INST/installer.img" | cut -d' ' -f1)
sed -i -e "s|^INSTALLER_DIR=.*|INSTALLER_DIR=installer/$KREL|" \
       -e "s|^SHA_VMLINUZ=.*|SHA_VMLINUZ=$sv|" -e "s|^SHA_INSTALLER=.*|SHA_INSTALLER=$si|" "$ROOT/synodeb-setup.sh"
cp "$ROOT/synodeb-setup.sh" "$ROOT/build/pages/synodeb-setup.sh"
find "$ROOT/build/pages" -type f -not -path "*/.git/*" | sed "s|$ROOT/build/pages/||" | sort
