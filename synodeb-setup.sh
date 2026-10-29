#!/bin/bash
# synodeb-setup.sh — run on DSM (as root) on a Synology DS923+.
#
#   sudo bash synodeb-setup.sh                 menu
#   sudo bash synodeb-setup.sh install         back up the DOM and add the Debian installer to it;
#                                              after a reboot the installer is reached over SSH
#   sudo bash synodeb-setup.sh boot debian     make Debian the default boot entry again
#   sudo bash synodeb-setup.sh boot dsm        make DSM the default boot entry
#   sudo bash synodeb-setup.sh cancel          remove the installer from the DOM, DSM boots
#   sudo bash synodeb-setup.sh status          show the boot entries on the DOM
#   sudo bash synodeb-setup.sh restore FILE    write a DOM backup made by 'install' back
#   sudo bash synodeb-setup.sh uninstall       advanced: remove everything synodeb put on the DOM
#                                              (Debian boot entries, kernels, installer) and put the
#                                              original Synology boot configuration back
#
# install options: --yes (no questions), --network dsm|dhcp, --hostname NAME,
#   --lang en|it|de|fr|es|nl|pt, --backup-dir DIR, --local DIR (installer files from DIR
#   instead of the download), --repo URL (apt repository for the installed system),
#   --replace-debian (if the DOM has no room for the installer next to an installed Debian,
#   remove that Debian's kernels from the DOM instead of stopping).
#
# The DOM is the 128 MB USB flash module the DS923+ firmware always boots: synoboot1 (FAT,
# Synology GRUB and its SynoBootLoader.conf) and synoboot2 (ext2, the DSM kernel). DSM does not
# let it be mounted, so each partition is copied, the copy is mounted and changed, written back
# with dd and read back to verify it. Nothing of DSM is changed: the Synology boot entries stay,
# synodeb only adds its own entries and the /synodeb directory on synoboot2.
set -eu
umask 022

VERSION=1
BASE_URL=https://rylos.github.io/synodeb
# installer files of this version of the script (updated by tools/publish.sh)
INSTALLER_DIR=installer/6.18.59-1-ds923
SHA_VMLINUZ=71287fe503563696bf1daf380c9375d1aab84e0fa3d224290eda873b950bdf4d
SHA_INSTALLER=790ef5a8d5358f24d91e528d10d26f93719703048e8beac2f034a9ada3f42224

P1=${SYNOBOOT1:-/dev/synoboot1}      # overridable for tests only
P2=${SYNOBOOT2:-/dev/synoboot2}
DOM=${SYNOBOOT:-/dev/synoboot}
FAT_ID="9c 58 ee 10"                                       # 10EE-589C, as in SynoBootLoader.conf
EXT_UUID="45 e5 b0 7d 47 83 48 67 a3 69 f9 9c 0c d1 e6 10" # 45e5b07d-4783-4867-a369-f99c0cd1e610
P2UUID=45e5b07d-4783-4867-a369-f99c0cd1e610
CONF_REL=EFI/boot/SynoBootLoader.conf
BEGIN='### synodeb begin'
END='### synodeb end'

# output never fails the script: after a dropped SSH session the terminal is gone
say() { printf '%s\n' "$*" 2>/dev/null || true; }
die() { printf 'synodeb-setup: %s\n' "$*" >&2 2>/dev/null || true; exit 1; }
ask() { # question default -> answer
    if [ "$YES" = 1 ]; then echo "$2"; return; fi
    printf '%s [%s]: ' "$1" "$2" > /dev/tty
    read -r a < /dev/tty || a=
    echo "${a:-$2}"
}
ask_tty() { # question default -> answer, asked even with --yes (default without a terminal)
    { printf '%s [%s]: ' "$1" "$2" > /dev/tty; } 2>/dev/null || { echo "$2"; return; }
    read -r a < /dev/tty || a=
    echo "${a:-$2}"
}
sha() { sha256sum "$1" | cut -d' ' -f1; }
bytes() { od -An -tx1 -j "$2" -N "$3" "$1" | tr -s ' \n' '  ' | sed 's/^ //; s/ $//'; }
size_of() { # block device or file
    if [ -b "$1" ]; then blockdev --getsize64 "$1"; else stat -c %s "$1"; fi
}
# read a device from the device itself, not from the page cache (O_DIRECT if it works)
DIRECT=0
rd() {
    if [ -b "$1" ]; then
        blockdev --flushbufs "$1" 2>/dev/null || true
        if [ "$DIRECT" = 1 ]; then dd if="$1" bs=64k iflag=direct 2>/dev/null; return; fi
    fi
    dd if="$1" bs=64k 2>/dev/null
}
rd_sha() { rd "$1" | sha256sum | cut -d' ' -f1; }
valid_host() { # RFC 1123 label
    [[ $1 =~ ^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$ ]]
}
valid_repo() { # goes into the GRUB config and a sed command of the installer: no quoting needed
    re='^https?://[A-Za-z0-9._~:/@%+=,-]+$'
    [[ $1 =~ $re ]]
}

# ---------------------------------------------------------------- signals
# A signal while the DOM is written would leave it half written: during the writes HUP
# (dropped SSH session), INT, TERM and PIPE are ignored, also by dd.
CRIT=0
traps() {
    trap cleanup EXIT
    trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; trap - PIPE
}
critical() { CRIT=$((CRIT + 1)); trap '' HUP INT TERM PIPE; }
uncritical() { CRIT=$((CRIT - 1)); [ "$CRIT" -gt 0 ] || { CRIT=0; traps; }; }

# ---------------------------------------------------------------- checks
# enough to write a backup back: works on a damaged DOM
preflight_dom() {
    [ "$(id -u)" = 0 ] || die "run as root: sudo bash $0"
    if [ "${SYNODEB_TEST:-0}" != 1 ]; then
        [ -f /etc.defaults/VERSION ] || die "this is not DSM: run this script on the NAS, under DSM"
        grep -q 'DS923+' /proc/sys/kernel/syno_hw_version 2>/dev/null ||
            die "this NAS is not a DS923+ ($(cat /proc/sys/kernel/syno_hw_version 2>/dev/null))"
    fi
    for t in dd od sha256sum mount umount blockdev sed awk mktemp; do
        command -v $t >/dev/null 2>&1 || die "missing tool: $t"
    done
    [ -b "$DOM" ] || die "$DOM not found: is the DOM (synoboot) present?"
    for p in "$DOM" "$P1" "$P2"; do
        ! grep -q "^$p " /proc/mounts || die "$p is mounted: stopping"
    done
    if dd if="$DOM" of=/dev/null bs=64k count=1 iflag=direct 2>/dev/null; then DIRECT=1; fi
}
preflight() {
    preflight_dom
    for p in "$P1" "$P2"; do
        [ -b "$p" ] || die "$p not found: is the DOM (synoboot) present?"
    done
    [ "$(bytes "$P1" 39 4)" = "$FAT_ID" ] || [ "$(bytes "$P1" 67 4)" = "$FAT_ID" ] ||
        die "$P1 is not the Synology boot partition (FAT volume id 10EE-589C expected)"
    [ "$(bytes "$P2" 1080 2)" = "53 ef" ] && [ "$(bytes "$P2" 1128 16)" = "$EXT_UUID" ] ||
        die "$P2 is not the Synology kernel partition (ext2 $P2UUID expected)"
    s1=$(size_of "$P1"); s2=$(size_of "$P2")
    [ "$s1" -ge 16777216 ] && [ "$s2" -ge 67108864 ] || die "unexpected DOM partition sizes ($s1, $s2)"
}

T=
cleanup() {
    [ -n "$T" ] || return 0
    umount "$T/m1" 2>/dev/null || true
    umount "$T/m2" 2>/dev/null || true
    if grep -q " $T/" /proc/mounts; then
        say "synodeb-setup: $T is still mounted, not removed" >&2
        return 0
    fi
    rm -rf --one-file-system "$T" 2>/dev/null || rm -rf "$T"
    T=
}
traps
workdir() {
    T=$(mktemp -d "${TMPDIR:-/tmp}/synodeb.XXXXXX")
    traps                       # also inside the subshells of the menu
    mkdir "$T/m1" "$T/m2"
    avail=$(df -Pk "$T" | awk 'NR == 2 { print $4 }')
    [ "$avail" -ge 250000 ] || die "not enough space in ${TMPDIR:-/tmp} ($avail KB, 250 MB needed): set TMPDIR to a folder on a volume"
}
# copy partition $1 to $T/<name>.img and mount the copy on $2 (fstype $3). The partition is
# read twice; without a DOM backup of this run the second read is kept as <name>.img.orig, to
# write it back if writing the partition fails.
READONLY=0
grab() {
    img=$T/$(basename "$1").img
    dd if="$1" of="$img" bs=64k 2>/dev/null || die "reading $1 failed"
    if [ -z "$BACKUP" ] && [ "$READONLY" = 0 ]; then
        rd "$1" > "$img.orig" || die "reading $1 failed"
        second=$(sha "$img.orig")
    else
        second=$(rd_sha "$1")
    fi
    sha "$img" > "$img.sha"
    [ "$(cat "$img.sha")" = "$second" ] ||
        die "reading $1 twice gave different data: the DOM may be failing, stopping"
    mount -t "$3" -o loop "$img" "$2" || die "mounting the copy of $1 failed"
}
# unmount $2, check the filesystem of the copy, write it to partition $1 and read it back
put() {
    img=$T/$(basename "$1").img
    sync; umount "$2" || die "unmounting the copy of $1 failed"
    case "$3" in
    ext2) if command -v e2fsck >/dev/null 2>&1; then e2fsck -fn "$img" >/dev/null 2>&1 || die "the modified copy of $1 does not pass e2fsck: DOM unchanged"; fi ;;
    vfat) if command -v fsck.vfat >/dev/null 2>&1; then fsck.vfat -n "$img" >/dev/null 2>&1 || die "the modified copy of $1 does not pass fsck.vfat: DOM unchanged"; fi ;;
    esac
    [ "$(size_of "$img")" = "$(size_of "$1")" ] || die "size of the copy of $1 changed: DOM unchanged"
    [ "$(rd_sha "$1")" = "$(cat "$img.sha")" ] ||
        die "$1 changed since it was read (is another program writing the DOM?): DOM unchanged"
    critical
    dd if="$img" of="$1" bs=64k conv=fsync 2>/dev/null || write_failed "$1"
    sync
    [ "$(rd_sha "$1")" = "$(sha "$img")" ] || write_failed "$1"
    sha "$img" > "$img.sha"
    uncritical
}
# the DOM backup of this run, else the copy of the partition read before the change
write_failed() {
    say "" >&2
    say "ERROR: writing $1 failed or it did not read back correctly." >&2
    if [ -n "$BACKUP" ]; then
        src=$BACKUP dst=$DOM
    else
        src=$T/$(basename "$1").img.orig dst=$1
    fi
    if [ -f "$src" ]; then
        say "Writing back $src ..." >&2
        if dd if="$src" of="$dst" bs=64k conv=fsync 2>/dev/null && sync && [ "$(rd_sha "$dst")" = "$(sha "$src")" ]; then
            say "The DOM is back as it was. Nothing changed." >&2
            exit 1
        fi
    fi
    say "RESTORE FAILED: do not reboot. Keep the DOM backup made by 'install' and write it back" >&2
    say "with: sudo bash $0 restore <backup>" >&2
    exit 1
}
# whole DOM to BACKUP_DIR (or the current folder), read twice and compared -> BACKUP
backup_dom() {
    bdir=${BACKUP_DIR:-$PWD}
    [ -d "$bdir" ] && [ -w "$bdir" ] || die "backup folder $bdir not writable"
    need=$(( $(size_of "$DOM") / 1024 + 1024 ))
    [ "$(df -Pk "$bdir" | awk 'NR == 2 { print $4 }')" -ge "$need" ] || die "not enough space in $bdir for the DOM backup ($need KB)"
    b=$bdir/synodeb-dom-backup-$(date +%Y%m%d-%H%M%S).img
    [ ! -e "$b" ] || b=${b%.img}-$$.img            # never overwrite an earlier backup
    say "Backing up the DOM to $b ..."
    ( umask 077; dd if="$DOM" of="$b" bs=64k 2>/dev/null ) || die "DOM backup failed"
    [ "$(sha "$b")" = "$(rd_sha "$DOM")" ] || die "the DOM backup does not match a second read of the DOM: stopping"
    ( umask 077; sha "$b" > "$b.sha256" )
    chmod 600 "$b" "$b.sha256"
    BACKUP=$b
}

# index of the menu entry titled $2 in conf $1 (GRUB counts from 0), empty if absent
entry() {
    awk -v t="$2" '/^[[:space:]]*menuentry /{ if (index($0, "\"" t "\"")) { print n; exit } n++ }' "$1"
}
set_default() { # conf title
    n=$(entry "$1" "$2")
    [ -n "$n" ] || die "no $2 boot entry on the DOM"
    sed "s/^set default='[0-9]*'/set default='$n'/" "$1" > "$1.tmp" && mv "$1.tmp" "$1"
    grep -q "^set default='$n'" "$1" || die "could not set the default boot entry"
}
# Synology conf without any synodeb entry (block markers or older DEBIAN*/INSTALLER entries)
strip_conf() {
    awk -v b="$BEGIN" -v e="$END" '
        index($0, b) == 1 { skip = 1; next }
        index($0, e) == 1 { skip = 0; next }
        /^menuentry "(DEBIAN|DEBIAN_OLD|INSTALLER)"/ { drop = 1 }
        skip || drop { if (drop && /^}/) drop = 0; next }
        { print }' "$1" | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'
}
# conf without the INSTALLER entry (and without its marker block once empty); DEBIAN* stay
drop_installer() {
    awk -v b="$BEGIN" -v e="$END" '
        index($0, b) == 1 { blk = 1; buf = ""; has = 0 }
        /^menuentry "INSTALLER"/ { drop = 1 }
        drop { if (/^}/) drop = 0; next }
        blk { buf = buf $0 "\n"; if (/^menuentry /) has = 1
              if (index($0, e) == 1) { blk = 0; if (has) printf "%s", buf }
              next }
        { print }' "$1"
}
check_conf() {
    [ -f "$1" ] || die "$CONF_REL missing on the DOM"
    grep -q '^set default=' "$1" || die "unexpected $CONF_REL (no 'set default')"
    grep -q 'syno_hw_version=DS923+' "$1" || die "unexpected $CONF_REL (not a DS923+ boot loader)"
    grep -q '^menuentry "SYNOLOGY_2"' "$1" || die "unexpected $CONF_REL (no SYNOLOGY_2 entry)"
}
# the Synology lines of a conf, without the ones synodeb changes (default, fallback)
syno_lines() { strip_conf "$1" | grep -v -e '^set default=' -e '^set fallback=' -e '^$'; }
# keep the original Synology conf as $1.dsm: an earlier copy stays while the Synology lines
# of the conf are the same, otherwise (e.g. DSM rewrote the conf) it is taken again from $1,
# verbatim if synodeb never touched it
save_original() {
    check_conf "$1"
    if [ -f "$1.dsm" ] && grep -q 'syno_hw_version=DS923+' "$1.dsm" &&
       [ "$(syno_lines "$1" | sha256sum)" = "$(syno_lines "$1.dsm" | sha256sum)" ]; then
        check_conf "$1.dsm"
        return
    fi
    if grep -q -e "^$BEGIN" -e '^menuentry "DEBIAN' -e '^menuentry "INSTALLER"' "$1"; then
        strip_conf "$1" > "$1.dsm"
    else
        cp "$1" "$1.dsm"
    fi
    check_conf "$1.dsm"
}
show_status() { # conf mounted dir of synoboot2
    d=$(sed -n "s/^set default='\([0-9]*\)'.*/\1/p" "$1" | head -n 1)
    say "Boot entries on the DOM:"
    awk -v d="$d" '/^[[:space:]]*menuentry /{ t = $0; sub(/^[^"]*"/, "", t); sub(/".*/, "", t)
        printf "  %d %s%s\n", n, t, (n == d ? "   <- default" : ""); n++ }' "$1"
    if [ -d "$2/synodeb" ]; then
        say "Files in synoboot2:/synodeb:"; ls -la "$2/synodeb" | sed 1d | sed 's/^/  /'
    fi
    say "Free space on synoboot2: $(df -Pk "$2" | awk 'NR == 2 { print $4 }') KB"
}

# ---------------------------------------------------------------- network
dsm_net() {
    IFACE=$(ip -4 route show default 2>/dev/null | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }')
    [ -n "$IFACE" ] || die "no default route on DSM: is the NAS on the network?"
    # the port the installer uses (it knows nothing of DSM's bonds and Open vSwitch bridges)
    DEV=$IFACE NETWARN=
    MAC=$(cat "/sys/class/net/$IFACE/address")
    case "$IFACE" in
    ovs_*)
        [ ! -e "/sys/class/net/${IFACE#ovs_}" ] || DEV=${IFACE#ovs_}
        MAC=$(cat "/sys/class/net/$DEV/address")
        NETWARN="DSM uses Open vSwitch ($IFACE): the installer and Debian use the port $DEV directly." ;;
    bond*)
        s=$(awk '{ print $1 }' "/sys/class/net/$IFACE/bonding/slaves" 2>/dev/null)
        if [ -n "$s" ]; then
            DEV=$s
            MAC=$(cat "/sys/class/net/$s/bonding_slave/perm_hwaddr" 2>/dev/null || cat "/sys/class/net/$s/address")
        fi
        NETWARN="DSM uses the bond $IFACE: the installer and Debian use the port $DEV only, without link
aggregation. If the switch ports are set up for link aggregation (LACP), the installer may get
no network: connect $DEV to a port without it, or remove the bond in DSM first." ;;
    esac
    CIDR=$(ip -4 -o addr show dev "$IFACE" scope global | awk '{ print $4; exit }')
    IP=${CIDR%/*}; PREFIX=${CIDR#*/}
    GW=$(ip -4 route show default | awk '{ print $3; exit }')
    DNS=$(awk '$1 == "nameserver" && $2 !~ /:/ && $2 !~ /^127\./ { print $2; exit }' /etc/resolv.conf)
    [ -n "$DNS" ] || DNS=$GW
    m=$(( 0xffffffff ^ ((1 << (32 - PREFIX)) - 1) ))
    MASK="$(( (m >> 24) & 255 )).$(( (m >> 16) & 255 )).$(( (m >> 8) & 255 )).$(( m & 255 ))"
    # does DSM get its address from DHCP?
    DSM_DHCP=0
    for f in "/etc/sysconfig/network-scripts/ifcfg-$IFACE" "/etc/sysconfig/network-scripts/ifcfg-$DEV"; do
        [ -f "$f" ] || continue
        if grep -qi "^BOOTPROTO=[\"']*dhcp" "$f"; then DSM_DHCP=1; fi
        break
    done
}

# ---------------------------------------------------------------- commands
cmd_install() {
    preflight
    dsm_net
    HOST=${HOSTNAME_OPT:-$(hostname | tr 'A-Z_' 'a-z-' | tr -cd 'a-z0-9-' | sed 's/^-*//; s/-*$//' | cut -c1-63)}
    valid_host "$HOST" || HOST=debian
    say "synodeb $VERSION — Debian installer for the Synology DS923+"
    say ""
    say "DSM network: $IFACE ($MAC) $CIDR, gateway $GW, DNS $DNS$([ "$DSM_DHCP" = 1 ] && echo ', from DHCP')"
    [ -z "$NETWARN" ] || say "$NETWARN"
    netdef=dsm
    [ "$DSM_DHCP" = 0 ] || netdef=dhcp
    if [ -z "$NETWORK" ]; then
        say "Network for the installer:"
        say "  dsm   the same address DSM has now ($IP, static)"
        say "  dhcp  an address from your DHCP server"
        [ "$DSM_DHCP" = 0 ] || say "DSM gets its address from DHCP: 'dhcp' is the default ('dsm' would make the"
        [ "$DSM_DHCP" = 0 ] || say "current lease $IP a static address of Debian)."
        NETWORK=$(ask "Choice" $netdef)
    elif [ "$NETWORK" = dsm ] && [ "$DSM_DHCP" = 1 ]; then
        say "Note: DSM gets $IP from DHCP; with --network dsm it becomes a static address of Debian."
    fi
    case "$NETWORK" in dsm|dhcp) ;; *) die "network: dsm or dhcp" ;; esac
    HOST=$(ask "Host name of the Debian system" "$HOST")
    valid_host "$HOST" || die "host name '$HOST': letters, digits and '-' only (at most 63), not starting or ending with '-'"
    LANG_OPT=$(ask "Installer language (en it de fr es nl pt)" "${LANG_OPT:-en}")
    case "$LANG_OPT" in
    en) LOCALE=en_US.UTF-8 KEYMAP=us ;; it) LOCALE=it_IT.UTF-8 KEYMAP=it ;;
    de) LOCALE=de_DE.UTF-8 KEYMAP=de ;; fr) LOCALE=fr_FR.UTF-8 KEYMAP=fr ;;
    es) LOCALE=es_ES.UTF-8 KEYMAP=es ;; nl) LOCALE=nl_NL.UTF-8 KEYMAP=us ;;
    pt) LOCALE=pt_PT.UTF-8 KEYMAP=pt ;; *) die "language: en it de fr es nl pt" ;;
    esac
    PW=$(tr -dc 'abcdefghijkmnpqrstuvwxyzACDEFGHJKLMNPQRTUVWXY34679' < /dev/urandom 2>/dev/null | head -c 12)
    [ ${#PW} = 12 ] || die "could not generate a password"

    workdir
    # installer files: downloaded (or --local), checked against the hashes in this script
    if [ -n "$LOCAL" ]; then
        cp "$LOCAL/vmlinuz" "$LOCAL/installer.img" "$T/" || die "files not found in $LOCAL"
    else
        case "$SHA_VMLINUZ$SHA_INSTALLER" in *@*) die "this copy of the script has no installer hashes: download it from the release" ;; esac
        command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1 || die "curl or wget needed to download the installer"
        for f in vmlinuz installer.img; do
            say "Downloading $f ..."
            if command -v curl >/dev/null 2>&1; then
                curl -fL --retry 3 -o "$T/$f" "$BASE_URL/$INSTALLER_DIR/$f" || die "download of $f failed"
            else
                wget -O "$T/$f" "$BASE_URL/$INSTALLER_DIR/$f" || die "download of $f failed"
            fi
        done
        [ "$(sha "$T/vmlinuz")" = "$SHA_VMLINUZ" ] || die "vmlinuz: wrong checksum, download it again"
        [ "$(sha "$T/installer.img")" = "$SHA_INSTALLER" ] || die "installer.img: wrong checksum, download it again"
    fi

    backup_dom

    grab "$P1" "$T/m1" vfat
    C=$T/m1/$CONF_REL
    save_original "$C"
    grab "$P2" "$T/m2" ext2
    for f in zImage rd.gz; do
        [ -f "$T/m2/$f" ] || die "synoboot2 has no $f: unexpected DOM content, stopping"
    done

    # synoboot2: /synodeb/installer.vmlinuz and installer.img. The kernels of an installed
    # Debian stay, so it still boots if this installation is cancelled or fails.
    S=$T/m2/synodeb
    keep=0
    [ -z "$(entry "$C" DEBIAN)" ] || ! [ -f "$S/vmlinuz" ] || ! [ -f "$S/initrd.img" ] || keep=1
    rm -f "$S/installer.img" "$S/installer.vmlinuz" "$S"/*.tmp 2>/dev/null || true
    p2free() { df -Pk "$T/m2" | awk 'NR == 2 { print $4 }'; }
    need=$(( ( $(size_of "$T/vmlinuz") + $(size_of "$T/installer.img") ) / 1024 + 1024 ))
    if [ $keep = 1 ]; then
        say "Debian is installed on this NAS: its kernel stays on the DOM next to the installer."
        if [ "$(p2free)" -lt "$need" ] && [ -f "$S/vmlinuz.old" ]; then
            say "Not enough space for both Debian kernels: the previous one (DEBIAN_OLD) is removed."
            rm -f "$S/vmlinuz.old" "$S/initrd.img.old" "$S/VERSION.old"
        fi
        if [ "$(p2free)" -lt "$need" ]; then
            say ""
            say "The DOM has no room for the installer next to the Debian kernel ($need KB needed,"
            say "$(p2free) KB free). Going on removes the installed Debian from the DOM: it cannot boot"
            say "again until a new installation completes (the Debian disks are not touched)."
            [ "$REPLACE" = 1 ] || [ "$(ask_tty "Remove the installed Debian from the DOM? (yes/no)" no)" = yes ] ||
                die "nothing written (--replace-debian removes the installed Debian from the DOM)"
            keep=0
        fi
    fi
    [ $keep = 1 ] || rm -rf "$S"
    [ "$(p2free)" -ge "$need" ] || die "not enough space on synoboot2: $need KB needed, $(p2free) KB free (DOM unchanged)"
    mkdir -p "$S"
    cp "$T/vmlinuz" "$S/installer.vmlinuz"
    cp "$T/installer.img" "$S/installer.img"
    sync
    [ "$(sha "$S/installer.vmlinuz")" = "$(sha "$T/vmlinuz")" ] || die "copy of vmlinuz on synoboot2 is wrong (DOM unchanged)"
    [ "$(sha "$S/installer.img")" = "$(sha "$T/installer.img")" ] || die "copy of installer.img on synoboot2 is wrong (DOM unchanged)"

    # synoboot1: Synology entries (and the Debian ones, if kept) untouched, INSTALLER added
    # and made the default
    if [ "$NETWORK" = dsm ]; then
        net="netcfg/choose_interface=$MAC netcfg/disable_autoconfig=true netcfg/get_ipaddress=$IP netcfg/get_netmask=$MASK netcfg/get_gateway=$GW netcfg/get_nameservers=$DNS netcfg/confirm_static=true"
    else
        net="netcfg/choose_interface=$MAC"
    fi
    {
        if [ $keep = 1 ]; then drop_installer "$C"; else strip_conf "$C.dsm"; fi
        echo "$BEGIN (written by synodeb-setup.sh)"
        cat <<EOT
menuentry "INSTALLER" {
        search --fs-uuid --no-floppy --set=root $P2UUID
        linux /synodeb/installer.vmlinuz console=tty0 console=uart8250,mmio32,0xfedc9000,115200n8 debian-installer/locale=$LOCALE keyboard-configuration/xkb-keymap=$KEYMAP netcfg/get_hostname=$HOST netcfg/hostname=$HOST $net network-console/password=$PW network-console/password-again=$PW${REPO:+ synodeb.repo=$REPO}
        initrd /synodeb/installer.img
}
EOT
        echo "$END"
    } > "$C.new"
    set_default "$C.new" INSTALLER
    mv "$C.new" "$C"
    rm -f "$T/m1/EFI/boot/synodeb-trial"

    say ""
    say "About to write the DOM:"
    say "  synoboot2: /synodeb/installer.vmlinuz and /synodeb/installer.img$([ $keep = 1 ] && echo ' (the Debian kernel stays)')"
    say "  synoboot1: boot entry INSTALLER, made the default (Synology$([ $keep = 1 ] && echo ' and Debian') entries unchanged)"
    [ "$(ask "Write the DOM now? (yes/no)" no)" = yes ] || [ "$YES" = 1 ] || die "nothing written"
    critical
    put "$P2" "$T/m2" ext2
    put "$P1" "$T/m1" vfat
    uncritical
    say ""
    say "Done. The DOM is written and verified."
    say ""
    say "DOM backup:   $BACKUP"
    say "              copy it off the NAS: if anything goes wrong it brings DSM back with"
    say "              sudo bash synodeb-setup.sh restore <file>"
    say ""
    say "Next steps:"
    say "  1. Reboot the NAS:   sudo reboot"
    say "  2. After 2-3 minutes the NAS gives one long beep: the installer is ready."
    if [ "$NETWORK" = dsm ]; then
        say "  3. Connect:          ssh installer@$IP"
    else
        say "  3. Connect:          ssh installer@<address>   (look up '$HOST' in your router / DHCP server)"
    fi
    say "     Password:         $PW   (one-time, valid only for this installation)"
    say "  4. Follow the Debian installer. Choose the disks for Debian in the partitioning step:"
    say "     keep the DSM disks untouched to keep DSM, or erase them to use them for Debian."
    say "     The DOM (128 MB, 'synoboot') is locked in the partitioner: never change it."
    if [ "$NETWORK" = dsm ]; then
        say "  5. At the end the NAS reboots into Debian: ssh <your user>@$IP"
    else
        say "  5. At the end the NAS reboots into Debian: ssh <your user>@<address> (DHCP: the"
        say "     address may differ from the installer's, look up '$HOST' in your router)"
    fi
    say ""
    say "If there is no beep within 20 minutes (no network), the NAS reboots into DSM by itself."
    say "The installer switches the DOM back to DSM as soon as it starts: until the installation"
    say "completes, any reboot brings DSM back."
    if [ $keep = 1 ]; then
        say "To boot the installed Debian again instead: sudo bash synodeb-setup.sh cancel, then"
        say "sudo bash synodeb-setup.sh boot debian."
    fi
}

cmd_boot() {
    preflight
    case "${1:-}" in debian) t=DEBIAN ;; dsm) t=SYNOLOGY_2 ;; *) die "usage: $0 boot debian|dsm" ;; esac
    workdir
    grab "$P1" "$T/m1" vfat
    C=$T/m1/$CONF_REL
    [ -f "$C" ] || die "$CONF_REL missing on the DOM"
    if [ $t = DEBIAN ]; then
        [ -n "$(entry "$C" DEBIAN)" ] || die "no Debian installed on this NAS (no DEBIAN boot entry)"
        READONLY=1 grab "$P2" "$T/m2" ext2
        [ -f "$T/m2/synodeb/vmlinuz" ] && [ -f "$T/m2/synodeb/initrd.img" ] ||
            die "the Debian kernel is missing on the DOM: boot Debian is not possible"
        umount "$T/m2"
    fi
    set_default "$C" $t
    rm -f "$T/m1/EFI/boot/synodeb-trial"
    put "$P1" "$T/m1" vfat
    say "Default boot entry: $t. Reboot to start it: sudo reboot"
}

cmd_cancel() {
    preflight
    workdir
    grab "$P1" "$T/m1" vfat
    C=$T/m1/$CONF_REL
    check_conf "$C"
    deb=$(entry "$C" DEBIAN)
    if [ -n "$deb" ]; then
        # keep an installed Debian: only the INSTALLER entry goes
        drop_installer "$C" > "$C.new"
    else
        strip_conf "$C" > "$C.new"
    fi
    set_default "$C.new" SYNOLOGY_2
    mv "$C.new" "$C"
    put "$P1" "$T/m1" vfat
    grab "$P2" "$T/m2" ext2
    if [ -n "$deb" ]; then
        rm -f "$T/m2/synodeb/installer.img" "$T/m2/synodeb/installer.vmlinuz"
    else
        rm -rf "$T/m2/synodeb"
    fi
    put "$P2" "$T/m2" ext2
    if [ -n "$deb" ]; then
        say "Installer removed, DSM is the default boot entry. The installed Debian stays:"
        say "sudo bash $0 boot debian makes it the default again."
    else
        say "Installer removed, DSM is the default boot entry."
    fi
}

cmd_uninstall() {
    preflight
    say "This removes Debian from the DOM: the DEBIAN, DEBIAN_OLD and INSTALLER boot entries,"
    say "synoboot2:/synodeb (kernels and installer) and the synodeb markers. The original"
    say "Synology boot configuration is put back and DSM boots. Debian on the disks is not"
    say "touched, but it can no longer boot until the installer runs again."
    say "For a byte-identical DOM use instead: $0 restore <backup>"
    [ "$(ask "Remove Debian from the DOM? (yes/no)" no)" = yes ] || [ "$YES" = 1 ] || die "nothing written"
    workdir
    backup_dom
    say "DOM backup (before uninstall): $BACKUP"
    grab "$P1" "$T/m1" vfat
    C=$T/m1/$CONF_REL
    if [ -f "$C.dsm" ]; then
        check_conf "$C.dsm"
        cp "$C.dsm" "$C"
    else
        check_conf "$C"
        strip_conf "$C" > "$C.new"; mv "$C.new" "$C"
    fi
    grep -q '^set default=' "$C" || die "unexpected conf"
    rm -f "$C.dsm" "$C.tmp" "$C.new" "$T/m1/EFI/boot/synodeb-trial" "$T/m1/EFI/boot/synodeb-pending"
    grab "$P2" "$T/m2" ext2
    rm -rf "$T/m2/synodeb"
    critical
    put "$P1" "$T/m1" vfat
    put "$P2" "$T/m2" ext2
    uncritical
    say "Done: the DOM boots DSM with its original configuration, no synodeb file is left."
}

cmd_status() {
    preflight
    workdir
    READONLY=1
    grab "$P1" "$T/m1" vfat
    grab "$P2" "$T/m2" ext2
    show_status "$T/m1/$CONF_REL" "$T/m2"
}

# byte offset of partition 1 in a DOM image (GPT or MBR), empty if there is none
img_p1_off() {
    if [ "$(bytes "$1" 450 1)" = ee ]; then
        [ "$(bytes "$1" 512 8)" = "45 46 49 20 50 41 52 54" ] || return 0    # "EFI PART"
        o=1056          # GPT: first entry at LBA 2, its starting LBA (low 32 bits)
    else
        o=454           # MBR: starting LBA of the first entry
    fi
    read -r a b c d <<< "$(bytes "$1" $o 4)"
    [ -n "$d" ] || return 0
    lba=$(( 0x$d$c$b$a ))
    [ "$lba" = 0 ] || echo $(( lba * 512 ))
}
cmd_restore() {
    preflight_dom                       # not preflight: the DOM may be damaged
    f=${1:-}
    [ -n "$f" ] && [ -f "$f" ] || die "usage: $0 restore synodeb-dom-backup-<date>.img"
    fs=$(size_of "$f") ds=$(size_of "$DOM")
    [ "$fs" = "$ds" ] || die "$f ($fs bytes) is not the size of the DOM ($ds bytes): not a backup of this DOM"
    off=$(img_p1_off "$f")
    [ -n "$off" ] && [ "$off" -lt "$fs" ] || die "$f has no partition table: not a DOM backup"
    [ "$(bytes "$f" $((off + 39)) 4)" = "$FAT_ID" ] || [ "$(bytes "$f" $((off + 67)) 4)" = "$FAT_ID" ] ||
        die "$f is not a DOM backup (no Synology boot partition in it)"
    if [ -f "$f.sha256" ]; then
        [ "$(sha "$f")" = "$(cat "$f.sha256")" ] || die "$f does not match $f.sha256: damaged backup"
    fi
    say "This writes $f back to the whole DOM ($DOM)."
    [ "$(ask "Continue? (yes/no)" no)" = yes ] || [ "$YES" = 1 ] || die "nothing written"
    critical
    dd if="$f" of="$DOM" bs=64k conv=fsync 2>/dev/null || die "writing the DOM failed: try again, do not reboot"
    sync
    [ "$(rd_sha "$DOM")" = "$(sha "$f")" ] ||
        die "the DOM does not read back as the backup: try again, do not reboot"
    uncritical
    say "DOM restored and verified."
}

# ---------------------------------------------------------------- main
YES=0 REPLACE=0 NETWORK='' HOSTNAME_OPT='' LANG_OPT='' BACKUP_DIR='' LOCAL='' REPO='' BACKUP=''

# one line about the DOM for the menu header
summary() {
    ( workdir
      READONLY=1
      grab "$P1" "$T/m1" vfat
      C=$T/m1/$CONF_REL
      d=$(sed -n "s/^set default='\([0-9]*\)'.*/\1/p" "$C" | head -n 1)
      def=$(awk -v d="$d" '/^[[:space:]]*menuentry /{ if (n++ == d) { sub(/^[^"]*"/, ""); sub(/".*/, ""); print } }' "$C")
      deb=no; [ -n "$(entry "$C" DEBIAN)" ] && deb=yes
      inst=no; [ -n "$(entry "$C" INSTALLER)" ] && inst=yes
      case "$def" in SYNOLOGY_*) def=DSM ;; DEBIAN) def=Debian ;; DEBIAN_OLD) def="Debian (previous kernel)" ;; INSTALLER) def="Debian installer" ;; esac
      printf 'boots: %s   Debian on the DOM: %s   installer ready: %s\n' "${def:-?}" "$deb" "$inst" ) 2>/dev/null ||
        echo "DOM state unknown"
}

# menu: a command runs in a subshell; when it fails or is declined the menu stays
run() {
    set +e
    ( set -e; "$@" )
    rc=$?
    set -e
    [ $rc = 0 ] || say "(not completed)"
}
menu() {
    preflight
    if [ -t 1 ]; then B=$'\e[1m' D=$'\e[2m' C=$'\e[36m' N=$'\e[0m'; else B='' D='' C='' N=''; fi
    while true; do
        say ""
        say "${B}synodeb $VERSION${N} — Debian for the Synology DS923+"
        say "${D}$(summary)${N}"
        say ""
        say "  ${C}1${N}) Install Debian        add the Debian installer to the DOM (reached over SSH)"
        say "  ${C}2${N}) Boot Debian           make Debian the default boot entry"
        say "  ${C}3${N}) Boot DSM              make DSM the default boot entry"
        say "  ${C}4${N}) Status                show the boot entries on the DOM"
        say "  ${C}5${N}) Cancel installer      remove the installer, DSM boots"
        say "  ${C}6${N}) Restore a DOM backup  write a backup made by 'Install Debian' back"
        say "  ${C}7${N}) Uninstall (advanced)  remove everything synodeb put on the DOM"
        say "  ${C}q${N}) Quit"
        say ""
        read -r -p "Choice: " c < /dev/tty || exit 0
        say ""
        case "$c" in
        1) run cmd_install ;;
        2) run cmd_boot debian ;;
        3) run cmd_boot dsm ;;
        4) run cmd_status ;;
        5) run cmd_cancel ;;
        6) read -r -p "Backup file: " f < /dev/tty || f=; run cmd_restore "$f" ;;
        7) run cmd_uninstall ;;
        q|Q|"") exit 0 ;;
        *) say "?" ;;
        esac
    done
}

# options anywhere on the command line, before or after the command
optval() { # option value
    [ $# -ge 2 ] && [ -n "$2" ] || die "option $1 needs a value"
    case "$2" in -*) die "option $1 needs a value (got $2)" ;; esac
}
cmd='' args=()
while [ $# -gt 0 ]; do
    case "$1" in
    --yes|-y) YES=1 ;;
    --replace-debian) REPLACE=1 ;;
    --network|--hostname|--lang|--backup-dir|--local|--repo)
        optval "$@"
        case "$1" in
        --network) NETWORK=$2 ;;
        --hostname) HOSTNAME_OPT=$2
            valid_host "$2" || die "--hostname $2: letters, digits and '-' only (at most 63), not starting or ending with '-'" ;;
        --lang) LANG_OPT=$2 ;;
        --backup-dir) BACKUP_DIR=$2 ;;
        --local) LOCAL=$2 ;;
        --repo) REPO=$2
            valid_repo "$2" || die "--repo $2: an http:// or https:// URL without spaces, quotes or shell characters" ;;
        esac
        shift ;;
    -h|--help) cmd=help ;;
    -*) die "unknown option $1" ;;
    *) if [ -z "$cmd" ]; then cmd=$1; else args+=("$1"); fi ;;
    esac
    shift
done
set -- "${args[@]+"${args[@]}"}"
noargs() { [ $# = 0 ] || die "unexpected argument: $1"; }
case "${cmd:-menu}" in
menu) noargs "$@"; menu ;;
install) noargs "$@"; cmd_install ;;
boot) [ $# -le 1 ] || die "usage: $0 boot debian|dsm"; cmd_boot "$@" ;;
cancel) noargs "$@"; cmd_cancel ;;
status) noargs "$@"; cmd_status ;;
restore) [ $# -le 1 ] || die "usage: $0 restore FILE"; cmd_restore "$@" ;;
uninstall) noargs "$@"; cmd_uninstall ;;
help) sed -n '2,20p' "$0" ;;
*) die "unknown command $cmd (install, boot, cancel, status, restore, uninstall)" ;;
esac
