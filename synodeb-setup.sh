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
#   instead of the download), --repo URL (apt repository for the installed system).
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
# Model profile: everything that is specific to one model. The script must stay a single
# downloadable file, so the profiles live here. Only the DS923+ is supported.
model_profile() { # model (from syno_hw_version)
    case "$1" in
    DS923+)
        MODEL=DS923+
        FAT_ID="9c 58 ee 10"                                       # 10EE-589C, as in SynoBootLoader.conf
        FAT_ID_TXT=10EE-589C
        EXT_UUID="45 e5 b0 7d 47 83 48 67 a3 69 f9 9c 0c d1 e6 10" # 45e5b07d-4783-4867-a369-f99c0cd1e610
        P2UUID=45e5b07d-4783-4867-a369-f99c0cd1e610
        P1_MIN=16777216 P2_MIN=67108864                           # minimum DOM partition sizes
        CONSOLE="console=tty0 console=uart8250,mmio32,0xfedc9000,115200n8"
        ;;
    *) return 1 ;;
    esac
}
# the model DSM runs on: /proc/sys/kernel/syno_hw_version, else syno_hw_version= on the
# kernel command line
hw_version() {
    v=$(cat /proc/sys/kernel/syno_hw_version 2>/dev/null) ||
        v=$(tr ' ' '\n' < /proc/cmdline | sed -n 's/^syno_hw_version=//p' | head -n 1)
    echo "$v"
}
model_profile DS923+
CONF_REL=EFI/boot/SynoBootLoader.conf
BEGIN='### synodeb begin'
END='### synodeb end'

say() { printf '%s\n' "$*"; }
die() { printf 'synodeb-setup: %s\n' "$*" >&2; exit 1; }
ask() { # question default -> answer
    if [ "$YES" = 1 ]; then echo "$2"; return; fi
    printf '%s [%s]: ' "$1" "$2" > /dev/tty
    read -r a < /dev/tty || a=
    echo "${a:-$2}"
}
sha() { sha256sum "$1" | cut -d' ' -f1; }
bytes() { od -An -tx1 -j "$2" -N "$3" "$1" | tr -s ' \n' '  ' | sed 's/^ //; s/ $//'; }
size_of() { # block device or file
    if [ -b "$1" ]; then blockdev --getsize64 "$1"; else stat -c %s "$1"; fi
}

# ---------------------------------------------------------------- checks
preflight() {
    [ "$(id -u)" = 0 ] || die "run as root: sudo bash $0"
    if [ "${SYNODEB_TEST:-0}" != 1 ]; then
        [ -f /etc.defaults/VERSION ] || die "this is not DSM: run this script on the NAS, under DSM"
        hw=$(hw_version)
        case "$hw" in *DS923+*) hw=DS923+ ;; esac
        model_profile "$hw" || die "this NAS is not a DS923+ ($hw)"
    fi
    for t in dd od sha256sum mount umount blockdev sed awk mktemp; do
        command -v $t >/dev/null 2>&1 || die "missing tool: $t"
    done
    for p in "$P1" "$P2"; do
        [ -b "$p" ] || die "$p not found: is the DOM (synoboot) present?"
        ! grep -q "^$p " /proc/mounts || die "$p is mounted: stopping"
    done
    [ "$(bytes "$P1" 39 4)" = "$FAT_ID" ] || [ "$(bytes "$P1" 67 4)" = "$FAT_ID" ] ||
        die "$P1 is not the Synology boot partition (FAT volume id $FAT_ID_TXT expected)"
    [ "$(bytes "$P2" 1080 2)" = "53 ef" ] && [ "$(bytes "$P2" 1128 16)" = "$EXT_UUID" ] ||
        die "$P2 is not the Synology kernel partition (ext2 $P2UUID expected)"
    s1=$(size_of "$P1"); s2=$(size_of "$P2")
    [ "$s1" -ge "$P1_MIN" ] && [ "$s2" -ge "$P2_MIN" ] || die "unexpected DOM partition sizes ($s1, $s2)"
}

T=
cleanup() {
    [ -n "$T" ] || return 0
    umount "$T/m1" 2>/dev/null || true
    umount "$T/m2" 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
workdir() {
    T=$(mktemp -d "${TMPDIR:-/tmp}/synodeb.XXXXXX")
    trap cleanup EXIT           # also inside the subshells of the menu
    mkdir "$T/m1" "$T/m2"
    avail=$(df -Pk "$T" | awk 'NR == 2 { print $4 }')
    [ "$avail" -ge 200000 ] || die "not enough space in ${TMPDIR:-/tmp} ($avail KB, 200 MB needed): set TMPDIR to a folder on a volume"
}
# copy partition $1 to $T/<name>.img and mount the copy on $2 (fstype $3)
grab() {
    img=$T/$(basename "$1").img
    dd if="$1" of="$img" bs=64k 2>/dev/null || die "reading $1 failed"
    [ "$(sha "$img")" = "$(dd if="$1" bs=64k 2>/dev/null | sha256sum | cut -d' ' -f1)" ] ||
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
    dd if="$img" of="$1" bs=64k conv=fsync 2>/dev/null || write_failed "$1"
    sync
    blockdev --flushbufs "$1" 2>/dev/null || true
    [ "$(dd if="$1" bs=64k 2>/dev/null | sha256sum | cut -d' ' -f1)" = "$(sha "$img")" ] || write_failed "$1"
}
write_failed() {
    say "" "ERROR: writing $1 failed or it did not read back correctly." >&2
    if [ -n "${BACKUP:-}" ]; then
        say "Restoring the DOM backup $BACKUP ..." >&2
        if dd if="$BACKUP" of="$DOM" bs=64k conv=fsync 2>/dev/null &&
           [ "$(dd if="$DOM" bs=64k count=$(( $(size_of "$BACKUP") / 65536 )) 2>/dev/null | sha256sum | cut -d' ' -f1)" = "$(sha "$BACKUP")" ]; then
            say "The DOM is back as it was. Nothing changed." >&2
        else
            say "RESTORE FAILED: do not reboot. Keep $BACKUP and ask for help." >&2
        fi
    fi
    exit 1
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
check_conf() {
    [ -f "$1" ] || die "$CONF_REL missing on the DOM"
    grep -q '^set default=' "$1" || die "unexpected $CONF_REL (no 'set default')"
    grep -q "syno_hw_version=$MODEL" "$1" || die "unexpected $CONF_REL (not a $MODEL boot loader)"
    grep -q '^menuentry "SYNOLOGY_2"' "$1" || die "unexpected $CONF_REL (no SYNOLOGY_2 entry)"
}
# keep the original Synology conf as $1.dsm (once): verbatim if synodeb never touched it
save_original() {
    if [ -f "$1.dsm" ] && grep -q "syno_hw_version=$MODEL" "$1.dsm"; then
        check_conf "$1.dsm"
        return
    fi
    check_conf "$1"
    if grep -q -e "^$BEGIN" -e '^menuentry "DEBIAN' -e '^menuentry "INSTALLER"' "$1"; then
        strip_conf "$1" > "$1.dsm"
    else
        cp "$1" "$1.dsm"
    fi
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
    MAC=$(cat "/sys/class/net/$IFACE/address")
    CIDR=$(ip -4 -o addr show dev "$IFACE" scope global | awk '{ print $4; exit }')
    IP=${CIDR%/*}; PREFIX=${CIDR#*/}
    GW=$(ip -4 route show default | awk '{ print $3; exit }')
    DNS=$(awk '$1 == "nameserver" && $2 !~ /:/ && $2 !~ /^127\./ { print $2; exit }' /etc/resolv.conf)
    [ -n "$DNS" ] || DNS=$GW
    m=$(( 0xffffffff ^ ((1 << (32 - PREFIX)) - 1) ))
    MASK="$(( (m >> 24) & 255 )).$(( (m >> 16) & 255 )).$(( (m >> 8) & 255 )).$(( m & 255 ))"
}

# ---------------------------------------------------------------- commands
cmd_install() {
    preflight
    dsm_net
    HOST=${HOSTNAME_OPT:-$(hostname | tr 'A-Z_' 'a-z-' | tr -cd 'a-z0-9-')}
    [ -n "$HOST" ] || HOST=debian
    say "synodeb $VERSION — Debian installer for the Synology $MODEL"
    say ""
    say "DSM network: $IFACE ($MAC) $CIDR, gateway $GW, DNS $DNS"
    if [ -z "$NETWORK" ]; then
        say "Network for the installer:"
        say "  dsm   the same address DSM has now ($IP, static)"
        say "  dhcp  an address from your DHCP server"
        NETWORK=$(ask "Choice" dsm)
    fi
    case "$NETWORK" in dsm|dhcp) ;; *) die "network: dsm or dhcp" ;; esac
    HOST=$(ask "Host name of the Debian system" "$HOST")
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

    # backup of the whole DOM (once per run), read twice and compared
    BDIR=${BACKUP_DIR:-$PWD}
    [ -d "$BDIR" ] && [ -w "$BDIR" ] || die "backup folder $BDIR not writable"
    need=$(( $(size_of "$DOM") / 1024 + 1024 ))
    [ "$(df -Pk "$BDIR" | awk 'NR == 2 { print $4 }')" -ge "$need" ] || die "not enough space in $BDIR for the DOM backup ($need KB)"
    BACKUP=$BDIR/synodeb-dom-backup-$(date +%Y%m%d-%H%M%S).img
    say "Backing up the DOM to $BACKUP ..."
    dd if="$DOM" of="$BACKUP" bs=64k 2>/dev/null || die "DOM backup failed"
    [ "$(sha "$BACKUP")" = "$(dd if="$DOM" bs=64k 2>/dev/null | sha256sum | cut -d' ' -f1)" ] ||
        die "the DOM backup does not match a second read of the DOM: stopping"
    sha "$BACKUP" > "$BACKUP.sha256"
    chmod 600 "$BACKUP" "$BACKUP.sha256"

    # synoboot2: /synodeb with the installer kernel and initrd only
    grab "$P2" "$T/m2" ext2
    for f in zImage rd.gz; do
        [ -f "$T/m2/$f" ] || die "synoboot2 has no $f: unexpected DOM content, stopping"
    done
    rm -rf "$T/m2/synodeb"
    free=$(df -Pk "$T/m2" | awk 'NR == 2 { print $4 }')
    need=$(( ( $(size_of "$T/vmlinuz") + $(size_of "$T/installer.img") ) / 1024 + 1024 ))
    [ "$free" -ge "$need" ] || die "not enough space on synoboot2: $need KB needed, $free KB free (DOM unchanged)"
    mkdir "$T/m2/synodeb"
    cp "$T/vmlinuz" "$T/m2/synodeb/vmlinuz"
    cp "$T/installer.img" "$T/m2/synodeb/installer.img"
    sync
    for f in vmlinuz installer.img; do
        [ "$(sha "$T/m2/synodeb/$f")" = "$(sha "$T/$f")" ] || die "copy of $f on synoboot2 is wrong (DOM unchanged)"
    done

    # synoboot1: Synology entries untouched, INSTALLER added and made the default
    grab "$P1" "$T/m1" vfat
    C=$T/m1/$CONF_REL
    save_original "$C"
    if [ "$NETWORK" = dsm ]; then
        net="netcfg/choose_interface=$MAC netcfg/disable_autoconfig=true netcfg/get_ipaddress=$IP netcfg/get_netmask=$MASK netcfg/get_gateway=$GW netcfg/get_nameservers=$DNS netcfg/confirm_static=true"
    else
        net="netcfg/choose_interface=$MAC"
    fi
    {
        strip_conf "$C.dsm"
        echo "$BEGIN (written by synodeb-setup.sh)"
        cat <<EOT
menuentry "INSTALLER" {
        search --fs-uuid --no-floppy --set=root $P2UUID
        linux /synodeb/vmlinuz $CONSOLE debian-installer/locale=$LOCALE keyboard-configuration/xkb-keymap=$KEYMAP netcfg/get_hostname=$HOST netcfg/hostname=$HOST $net network-console/password=$PW network-console/password-again=$PW${REPO:+ synodeb.repo=$REPO}
        initrd /synodeb/installer.img
}
EOT
        echo "$END"
    } > "$C.new"
    set_default "$C.new" INSTALLER
    mv "$C.new" "$C"

    say ""
    say "About to write the DOM:"
    say "  synoboot2: /synodeb/vmlinuz and /synodeb/installer.img"
    say "  synoboot1: boot entry INSTALLER, made the default (Synology entries unchanged)"
    [ "$(ask "Write the DOM now? (yes/no)" no)" = yes ] || [ "$YES" = 1 ] || die "nothing written"
    put "$P2" "$T/m2" ext2
    put "$P1" "$T/m1" vfat
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
        grab "$P2" "$T/m2" ext2
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
    { strip_conf "$C"; } > "$C.new"
    if [ -n "$(entry "$C" DEBIAN)" ]; then
        # keep an installed Debian: only the INSTALLER entry goes
        awk '/^menuentry "INSTALLER"/ { drop = 1 } drop { if (/^}/) drop = 0; next } { print }' "$C" > "$C.new"
    fi
    set_default "$C.new" SYNOLOGY_2
    mv "$C.new" "$C"
    put "$P1" "$T/m1" vfat
    grab "$P2" "$T/m2" ext2
    if [ -n "$(entry "$C" DEBIAN)" ]; then
        rm -f "$T/m2/synodeb/installer.img"
    else
        rm -rf "$T/m2/synodeb"
    fi
    put "$P2" "$T/m2" ext2
    say "Installer removed, DSM is the default boot entry."
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
    BACKUP_DIR=${BACKUP_DIR:-$PWD}
    BACKUP=$BACKUP_DIR/synodeb-dom-backup-$(date +%Y%m%d-%H%M%S).img
    dd if="$DOM" of="$BACKUP" bs=64k 2>/dev/null || die "DOM backup failed"
    sha "$BACKUP" > "$BACKUP.sha256"
    chmod 600 "$BACKUP" "$BACKUP.sha256"
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
    put "$P1" "$T/m1" vfat
    grab "$P2" "$T/m2" ext2
    rm -rf "$T/m2/synodeb"
    put "$P2" "$T/m2" ext2
    say "Done: the DOM boots DSM with its original configuration, no synodeb file is left."
}

cmd_status() {
    preflight
    workdir
    grab "$P1" "$T/m1" vfat
    grab "$P2" "$T/m2" ext2
    show_status "$T/m1/$CONF_REL" "$T/m2"
}

cmd_restore() {
    preflight
    f=${1:-}
    [ -f "$f" ] || die "usage: $0 restore synodeb-dom-backup-<date>.img"
    [ "$(size_of "$f")" -le "$(size_of "$DOM")" ] || die "$f is larger than the DOM"
    if [ -f "$f.sha256" ]; then
        [ "$(sha "$f")" = "$(cat "$f.sha256")" ] || die "$f does not match $f.sha256: damaged backup"
    fi
    say "This writes $f back to the whole DOM ($DOM)."
    [ "$(ask "Continue? (yes/no)" no)" = yes ] || [ "$YES" = 1 ] || die "nothing written"
    dd if="$f" of="$DOM" bs=64k conv=fsync 2>/dev/null || die "writing the DOM failed"
    sync
    [ "$(dd if="$DOM" bs=64k count=$(( $(size_of "$f") / 65536 )) 2>/dev/null | sha256sum | cut -d' ' -f1)" = "$(sha "$f")" ] ||
        die "the DOM does not read back as the backup: try again, do not reboot"
    say "DOM restored and verified."
}

# ---------------------------------------------------------------- main
YES=0 NETWORK='' HOSTNAME_OPT='' LANG_OPT='' BACKUP_DIR='' LOCAL='' REPO='' BACKUP=''

# one line about the DOM for the menu header
summary() {
    ( workdir
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

menu() {
    preflight
    if [ -t 1 ]; then B=$'\e[1m' D=$'\e[2m' C=$'\e[36m' N=$'\e[0m'; else B='' D='' C='' N=''; fi
    while true; do
        say ""
        say "${B}synodeb $VERSION${N} — Debian for the Synology $MODEL"
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
        1) ( cmd_install ) ;;
        2) ( cmd_boot debian ) ;;
        3) ( cmd_boot dsm ) ;;
        4) ( cmd_status ) ;;
        5) ( cmd_cancel ) ;;
        6) read -r -p "Backup file: " f < /dev/tty; ( cmd_restore "$f" ) ;;
        7) ( cmd_uninstall ) ;;
        q|Q|"") exit 0 ;;
        *) say "?" ;;
        esac
    done
}
cmd=${1:-menu}
[ $# = 0 ] || shift
args=
while [ $# -gt 0 ]; do
    case "$1" in
    --yes|-y) YES=1 ;;
    --network) NETWORK=${2:-}; shift ;;
    --hostname) HOSTNAME_OPT=${2:-}; shift ;;
    --lang) LANG_OPT=${2:-}; shift ;;
    --backup-dir) BACKUP_DIR=${2:-}; shift ;;
    --local) LOCAL=${2:-}; shift ;;
    --repo) REPO=${2:-}; shift ;;
    -*) die "unknown option $1" ;;
    *) args="$args $1" ;;
    esac
    shift
done
# shellcheck disable=SC2086
set -- $args
case "$cmd" in
menu) menu ;;
install) cmd_install ;;
boot) cmd_boot "$@" ;;
cancel) cmd_cancel ;;
status) cmd_status ;;
restore) cmd_restore "$@" ;;
uninstall) cmd_uninstall ;;
-h|--help|help) sed -n '2,20p' "$0" ;;
*) die "unknown command $cmd (install, boot, cancel, status, restore, uninstall)" ;;
esac
