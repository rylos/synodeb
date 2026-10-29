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
# synology-ds923 is built from the committed tree (git archive HEAD), never from the working
# tree: a version already in the repository is kept if its content is the same, and refused
# if it differs (bump Version in pkg/synology-ds923/DEBIAN/control).
# Runs on the host; the repository is built in a debian:trixie container. The signing key is
# in $SYNODEB_GNUPG (default ~/.local/share/synodeb/gnupg), never in the project.
# SYNODEB_KEY: fingerprint of the signing key (default: the synodeb archive key; another key
# is only for tests, and skips the comparison with keys/synodeb-archive.asc).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
GNUPG=${SYNODEB_GNUPG:-$HOME/.local/share/synodeb/gnupg}
ARCHIVE_KEY=CEE2A6FA9657FB3CBD61EA074200ADB5550E6DEE
KEY=${SYNODEB_KEY:-$ARCHIVE_KEY}
[ -d "$GNUPG" ] || { echo "signing key not found in $GNUPG" >&2; exit 1; }
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
# build/out is written by the kernel build: only regular files go into the repository
if [ -n "$(find "$OUT" ! -type f ! -type d -print -quit)" ]; then
    echo "build/out holds something that is not a regular file:" >&2
    find "$OUT" ! -type f ! -type d >&2
    exit 1
fi

# synology-ds923 from the committed tree: content, file modes and dates depend only on the
# commit, so the same version always gives the same package
PKG=pkg/synology-ds923
[ "$(git -C "$ROOT" rev-parse --is-shallow-repository)" = false ] ||
    { echo "shallow clone: the package dates come from the git history (git fetch --unshallow)" >&2; exit 1; }
[ -z "$(git -C "$ROOT" status --porcelain -- "$PKG")" ] ||
    echo "note: uncommitted changes in $PKG are not published" >&2
PKGVER=$(git -C "$ROOT" show "HEAD:$PKG/DEBIAN/control" | sed -n 's/^Version: //p')
PKGCOMMIT=$(git -C "$ROOT" log -1 --format=%H HEAD -- "$PKG")
PKGEPOCH=$(git -C "$ROOT" log -1 --format=%ct HEAD -- "$PKG")
PKGYEARS=$(git -C "$ROOT" log --reverse --format=%ad --date=format:%Y HEAD -- "$PKG" | sed -n 1p)
[ "$PKGYEARS" = "$(date -u -d "@$PKGEPOCH" +%Y)" ] || PKGYEARS=$PKGYEARS-$(date -u -d "@$PKGEPOCH" +%Y)
mkdir -p "$ROOT/build"
git -C "$ROOT" -c tar.umask=022 archive --format=tar -o "$ROOT/build/synology-ds923.tar" HEAD "$PKG"

# shellcheck disable=SC2016  # the script runs in the container
SCRIPT='
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq apt-utils gpg dpkg-dev xz-utils >/dev/null </dev/null
P=/work/build/pages
R=$P/debian
mkdir -p $R/pool/main $R/dists/trixie/main/binary-amd64 $P/installer
export GNUPGHOME=/tmp/gnupg
cp -a /gnupg $GNUPGHOME; find $GNUPGHOME -type s -delete; chown -R root:root $GNUPGHOME; chmod 700 $GNUPGHOME
gpg --batch -q --list-secret-keys "$KEY" >/dev/null || { echo "signing key $KEY not found" >&2; exit 1; }

# synology-ds923
export SOURCE_DATE_EPOCH=$PKGEPOCH
B=/tmp/pkg; mkdir $B
tar -xf /work/build/synology-ds923.tar -C $B --strip-components=2
find $B -type d -exec chmod 0755 {} +
find $B -type f -perm /111 -exec chmod 0755 {} +
find $B -type f ! -perm /111 -exec chmod 0644 {} +
chmod 0600 $B/etc/synodeb/notify.conf        # may hold tokens and passwords
D=$B/usr/share/doc/synology-ds923
mkdir -p $D
cat > $D/copyright <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: synodeb
Source: https://github.com/rylos/synodeb

Files: *
Copyright: $PKGYEARS Marco Ziliani
License: GPL-2
 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License version 2 as published by
 the Free Software Foundation.
 .
 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 .
 On Debian systems, the complete text of the GNU General Public License
 version 2 can be found in /usr/share/common-licenses/GPL-2.
EOF
maint=$(sed -n "s/^Maintainer: //p" $B/DEBIAN/control)
{
    echo "synology-ds923 ($PKGVER) trixie; urgency=medium"
    echo
    echo "  * Built from commit $PKGCOMMIT"
    echo "    (https://github.com/rylos/synodeb/commits/$PKGCOMMIT/pkg/synology-ds923)."
    echo
    echo " -- $maint  $(date -u -R -d @$PKGEPOCH)"
} | gzip -9n > $D/changelog.gz
chmod 0644 $D/copyright $D/changelog.gz
(cd $B && find . -path ./DEBIAN -prune -o -type f -printf "%P\0" | LC_ALL=C sort -z | xargs -0 md5sum) > $B/DEBIAN/md5sums
chmod 0644 $B/DEBIAN/md5sums
find $B -exec touch -h -d @$PKGEPOCH {} +
NEW=/work/build/out/synology-ds923_${PKGVER}_all.deb
dpkg-deb --root-owner-group -Zxz -b $B $NEW >/dev/null
# what makes a package: control files, file list with modes and owners, file contents
debsum() {
    dpkg-deb --ctrl-tarfile "$1" | tar -xO
    dpkg-deb --fsys-tarfile "$1" | tar -tv | awk "{ \$4 = \$5 = \"\"; print }"
    dpkg-deb --fsys-tarfile "$1" | tar -xO | md5sum
}
OLD=$R/pool/main/synology-ds923_${PKGVER}_all.deb
if [ -f $OLD ]; then
    if [ "$(debsum $OLD | md5sum)" = "$(debsum $NEW | md5sum)" ]; then
        echo "synology-ds923 $PKGVER: already in the repository, unchanged"
    else
        echo "synology-ds923 $PKGVER is already in the repository with a different content:" >&2
        echo "bump Version in pkg/synology-ds923/DEBIAN/control" >&2
        diff <(debsum $OLD) <(debsum $NEW) | head -n 20 >&2 || true
        exit 1
    fi
else
    cp $NEW $R/pool/main/
fi
unset SOURCE_DATE_EPOCH

# metapackages of this series: only the new version (linux-image-ds923 has an epoch, so the
# file names do not sort by version)
metas="linux-image-ds923-$SERIES linux-headers-ds923-$SERIES"
[ "$SERIES" = longterm ] && metas="$metas linux-image-ds923 linux-headers-ds923"
for m in $metas; do
    rm -f $R/pool/main/${m}_*.deb
    cp /work/build/out/${m}_${KDEBVER}_amd64.deb $R/pool/main/
done
for f in linux-image-${KREL}_${KDEBVER}_amd64.deb linux-headers-${KREL}_${KDEBVER}_amd64.deb; do
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
gzip -9nkf dists/trixie/main/binary-amd64/Packages
xz -9kf dists/trixie/main/binary-amd64/Packages
# the old Release files out of the way, or the new Release lists them
mkdir /tmp/old-release
mv -f dists/trixie/Release dists/trixie/InRelease dists/trixie/Release.gpg /tmp/old-release/ 2>/dev/null || true
apt-ftparchive -o APT::FTPArchive::Release::Origin=synodeb -o APT::FTPArchive::Release::Label=synodeb \
    -o APT::FTPArchive::Release::Suite=trixie -o APT::FTPArchive::Release::Codename=trixie \
    -o APT::FTPArchive::Release::Architectures=amd64 -o APT::FTPArchive::Release::Components=main \
    -o "APT::FTPArchive::Release::Description=synodeb: Debian on the Synology DS923+" \
    release dists/trixie > /tmp/Release
mv /tmp/Release dists/trixie/Release
gpg --batch -q --yes --local-user "$KEY!" --clearsign -o dists/trixie/InRelease dists/trixie/Release
gpg --batch -q --yes --local-user "$KEY!" -abs -o dists/trixie/Release.gpg dists/trixie/Release
gpg --batch --export --armor "$KEY" > $P/synodeb-archive.asc
if [ "$KEY" = "$ARCHIVE_KEY" ] && [ -f /work/keys/synodeb-archive.asc ]; then
    show() { gpg --batch --show-keys --with-colons "$1" | grep -E "^(pub|sub|fpr|uid):"; }
    [ "$(show $P/synodeb-archive.asc)" = "$(show /work/keys/synodeb-archive.asc)" ] ||
        { echo "keys/synodeb-archive.asc differs from the signing key (expiry, subkeys, uids?): update it" >&2; exit 1; }
fi
# installer
if [ "$SKIP_INSTALLER" != 1 ]; then
    rm -rf $P/installer/$KREL; mkdir -p $P/installer/$KREL
    cp /work/build/out/installer/{vmlinuz,installer.img,SHA256SUMS,VERSION} $P/installer/$KREL/
    ls $P/installer | sort -V | head -n -2 | while read -r d; do rm -rf "$P/installer/$d"; done
fi
touch $P/.nojekyll
chown -R $HOSTUID:$HOSTUID $P $NEW
'
docker run --rm -v "$ROOT":/work -v "$GNUPG":/gnupg:ro -e KREL="$KREL" -e PKGVER="$PKGVER" \
    -e PKGCOMMIT="$PKGCOMMIT" -e PKGEPOCH="$PKGEPOCH" -e PKGYEARS="$PKGYEARS" \
    -e KEY="$KEY" -e ARCHIVE_KEY="$ARCHIVE_KEY" \
    -e KDEBVER="$KPKGVER" -e SERIES="$SERIES" -e HOSTUID="$(id -u)" -e SKIP_INSTALLER="${SKIP_INSTALLER:-0}" \
    debian:trixie bash -euo pipefail -c "$SCRIPT"
rm -f "$ROOT/build/synology-ds923.tar"
[ "${SKIP_INSTALLER:-0}" != 1 ] || exit 0
# checksums of this installer in synodeb-setup.sh
sv=$(sha256sum "$INST/vmlinuz" | cut -d' ' -f1)
si=$(sha256sum "$INST/installer.img" | cut -d' ' -f1)
sed -i -e "s|^INSTALLER_DIR=.*|INSTALLER_DIR=installer/$KREL|" \
       -e "s|^SHA_VMLINUZ=.*|SHA_VMLINUZ=$sv|" -e "s|^SHA_INSTALLER=.*|SHA_INSTALLER=$si|" "$ROOT/synodeb-setup.sh"
cp "$ROOT/synodeb-setup.sh" "$ROOT/build/pages/synodeb-setup.sh"
find "$ROOT/build/pages" -type f -not -path "*/.git/*" | sed "s|$ROOT/build/pages/||" | sort
