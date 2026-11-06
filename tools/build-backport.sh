#!/bin/bash
# Rebuilds a Debian source package from sid for trixie, for the synodeb apt repository, when
# trixie's version lacks something synodeb needs (e.g. NUT 2.8.5: drivers for UPS models that
# 2.8.1 does not know). The source comes from deb.debian.org through apt (signed Release),
# version <sid version>~synodeb13+N, so a newer Debian package replaces it.
#   tools/build-backport.sh nut [N]
# Output: build/out/backports/*.deb (no -dbgsym), then tools/publish.sh adds them.
# Conffiles under KEEP_CONF (default for nut: /etc/nut/) are put back as trixie ships them: an
# upgrade from trixie's version then never asks about a configuration the admin (or
# synodeb-ups setup) changed. Only sample configurations with comment changes are replaced.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:?usage: build-backport.sh SOURCE-PACKAGE [N]}
N=${2:-1}
case $SRC in *[!a-z0-9.+-]*) echo "invalid source package name: $SRC" >&2; exit 1 ;; esac
case $N in *[!0-9]*|'') echo "invalid rebuild number: $N" >&2; exit 1 ;; esac
case $SRC in nut) KEEP_CONF=${KEEP_CONF:-/etc/nut/} ;; *) KEEP_CONF=${KEEP_CONF:-} ;; esac
OUT=$ROOT/build/out/backports
mkdir -p "$OUT"
# shellcheck disable=SC2016  # the script runs in the container
docker run --rm -v "$OUT":/out -e SRC="$SRC" -e N="$N" -e KEEP_CONF="$KEEP_CONF" -e HOSTUID="$(id -u)" debian:trixie bash -euo pipefail -c '
export DEBIAN_FRONTEND=noninteractive
cat > /etc/apt/sources.list.d/sid-src.sources <<EOT
Types: deb-src
URIs: http://deb.debian.org/debian
Suites: sid
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
EOT
apt-get update -qq
apt-get install -y -qq build-essential devscripts equivs dpkg-dev >/dev/null
mkdir /src && cd /src
apt-get source -qq "$SRC/sid"
cd "$(find . -mindepth 1 -maxdepth 1 -type d | head -n 1)"
mk-build-deps -i -r -t "apt-get -y -qq --no-install-recommends" debian/control >/dev/null
ver=$(dpkg-parsechangelog -S Version)~synodeb13+$N
DEBEMAIL=508111+rylos@users.noreply.github.com DEBFULLNAME="Marco Ziliani" \
    dch -b -v "$ver" -D trixie --force-distribution "Rebuilt for Debian trixie (synodeb repository)."
DEB_BUILD_OPTIONS="nocheck parallel=$(nproc)" dpkg-buildpackage -b -us -uc > /out/build-$SRC.log 2>&1 ||
    { tail -n 40 /out/build-$SRC.log; exit 1; }
cd /src
rm -f ./*-dbgsym_*.deb /out/*_*~synodeb13+"$N"_*.deb
if [ -n "$KEEP_CONF" ]; then
    mkdir /trixie
    for d in /src/*.deb; do (cd /trixie && apt-get download -qq "$(dpkg-deb -f "$d" Package)" 2>/dev/null) || true; done
    for d in /src/*.deb; do
        pkg=$(dpkg-deb -f "$d" Package)
        old=$(find /trixie -name "${pkg}_*.deb" | head -n 1)
        [ -n "$old" ] || continue
        keep=$(dpkg-deb --ctrl-tarfile "$d" | tar -xO ./conffiles 2>/dev/null | grep "^$KEEP_CONF" || true)
        [ -n "$keep" ] || continue
        rm -rf /re /oldx && dpkg-deb -R "$d" /re && dpkg-deb -x "$old" /oldx
        for f in $keep; do
            [ -f "/oldx$f" ] || continue
            cmp -s "/oldx$f" "/re$f" && continue
            cat "/oldx$f" > "/re$f"
            sum=$(md5sum < "/re$f" | cut -d" " -f1)
            sed -i "s|^[0-9a-f]*  ${f#/}\$|$sum  ${f#/}|" /re/DEBIAN/md5sums
            echo "$pkg: $f as in trixie"
        done
        dpkg-deb -Zxz -b /re "$d" >/dev/null
    done
fi
cp ./*.deb /out/
chown -R "$HOSTUID:$HOSTUID" /out
ls /out
'
