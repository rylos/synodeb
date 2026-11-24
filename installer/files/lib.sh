# synodeb installer: helpers shared by early and finish (busybox sh in the installer)

# The DOM partitions by their volume ids (the same on every DS920+/DS923+), else partitions 1
# and 2 of the USB disk f400:f400 (Avoton: the ids differ per unit, synoboot2 is FAT)
DOM_P1=10EE-589C                               # synoboot1, FAT: EFI/boot/SynoBootLoader.conf
DOM_P2=45e5b07d-4783-4867-a369-f99c0cd1e610    # synoboot2, ext2 or FAT: kernel + initrd
DOM_MNT=/run/synodeb-dom
DOM_LOCK=/hd-media
LOG=/var/log/synodeb-installer.log

log() { echo "synodeb: $*" >> $LOG; logger -t synodeb -- "$*" 2>/dev/null || true; }

# --- front-panel microcontroller (uP), 9600 8N1: on r1000 (DS923+) the UART at MMIO
# 0xFEDCA000, on geminilake (DS920+) LPSS UART 0, PCI 00:18.0, on avoton (DS415+) ttyS1,
# I/O 0x2f8 (MohonPeak board) ---
up_dev() {
    if [ "$(cat /sys/class/dmi/id/product_name 2>/dev/null)" = MohonPeak ]; then
        [ "$(cat /sys/class/tty/ttyS1/port 2>/dev/null)" = 0x2F8 ] && { echo /dev/ttyS1; return 0; }
        return 1
    fi
    for t in /sys/class/tty/ttyS*; do
        b=$(tr a-f A-F < "$t/iomem_base" 2>/dev/null)
        [ "$b" = 0XFEDCA000 ] && { echo "/dev/${t##*/}"; return 0; }
        case "$(readlink -f "$t/device" 2>/dev/null)" in
        */0000:00:18.0/*)
            [ "$(cat /sys/bus/pci/devices/0000:00:18.0/vendor 2>/dev/null)" = 0x8086 ] &&
                { echo "/dev/${t##*/}"; return 0; } ;;
        esac
    done
    return 1
}
# up CMD...: '4' power LED on, '9' status LED green blinking, '8' green, ':' orange,
# '2' short beep, '3' long beep, 'V50' fans at 50 %
up() {
    d=$(up_dev) || return 0
    stty -F "$d" 9600 raw -echo 2>/dev/null || true
    # the busybox sleep of the installer takes whole seconds only
    for c in "$@"; do printf '%s' "$c" > "$d" 2>/dev/null; sleep 0.3 2>/dev/null || sleep 1; done
}

# --- DOM ---
dom_modules() { modprobe vfat 2>/dev/null; modprobe nls_cp437 2>/dev/null; modprobe nls_ascii 2>/dev/null; }
dom_disk() { # the USB disk f400:f400 (sdX)
    for b in /sys/class/block/sd*; do
        [ -e "$b/device" ] && [ ! -e "$b/partition" ] || continue
        u=$(readlink -f "$b/device")
        while [ -n "$u" ] && [ "$u" != /sys ]; do
            if [ -f "$u/idVendor" ]; then
                [ "$(cat "$u/idVendor")" = f400 ] && [ "$(cat "$u/idProduct")" = f400 ] &&
                    { echo "${b##*/}"; return 0; }
                break
            fi
            u=${u%/*}
        done
    done
    return 1
}
dom_dev() { # 1|2: device of synoboot1/synoboot2
    case $1 in 1) id=$DOM_P1 ;; *) id=$DOM_P2 ;; esac
    blkid -U "$id" 2>/dev/null && return 0
    d=$(dom_disk) && [ -b "/dev/$d$1" ] && { echo "/dev/$d$1"; return 0; }
    return 1
}
dom_fs() { # device: vfat or ext2
    fs=$(blkid -o value -s TYPE "$1" 2>/dev/null)
    case "$fs" in vfat|ext2) echo "$fs" ;; *) return 1 ;; esac
}
dom_mount() { # 1|2
    dev=$(dom_dev "$1") || return 1
    dom_modules
    fs=$(dom_fs "$dev") || return 1
    mkdir -p $DOM_MNT
    mount -t "$fs" "$dev" $DOM_MNT 2>/dev/null
}
dom_umount() { sync; umount $DOM_MNT 2>/dev/null; }
# make the menu entry titled $1 the default of the DOM GRUB
dom_default() {
    conf=$DOM_MNT/EFI/boot/SynoBootLoader.conf
    dom_mount 1 || { log "DOM synoboot1 not found"; return 1; }
    # entries counted as synodeb-setup.sh and synodeb-dom do: lines starting with menuentry
    # (GRUB 2), or with title (GRUB legacy, Avoton)
    lg=0
    grep -q '^set default=' "$conf" || ! grep -q '^default[[:space:]]' "$conf" || lg=1
    n=0; idx=
    while read -r l; do
        if [ $lg = 1 ]; then
            case "$l" in title\ *|title"	"*)
                t=$(printf '%s\n' "${l#title}" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')
                [ "$t" = "$1" ] && { idx=$n; break; }
                n=$((n + 1)) ;;
            esac
        else
            case "$l" in
            "menuentry \"$1\""*) idx=$n; break ;;
            "menuentry "*) n=$((n + 1)) ;;
            esac
        fi
    done < "$conf"
    if [ $lg = 1 ]; then
        dx="s/^default[[:space:]].*/default $idx/" dok="^default $idx\$"
    else
        dx="s/^set default='[0-9]*'/set default='$idx'/" dok="^set default='$idx'"
    fi
    rc=1
    if [ -n "$idx" ] && sed "$dx" "$conf" > "$conf.tmp" &&
       grep -q "$dok" "$conf.tmp" && mv -f "$conf.tmp" "$conf"; then
        rc=0; log "DOM default entry: $1 ($idx)"
    else
        rm -f "$conf.tmp"; log "could not make $1 the DOM default"
    fi
    dom_umount
    return $rc
}

# While the installer runs, both DOM partitions are mounted read-only on /hd-media: partman
# takes them for installation media, leaves the DOM out of guided partitioning and locks its
# partitions in manual partitioning. finish unmounts them before it writes the DOM.
dom_protect() {
    dom_modules
    for n in 1 2; do
        dev=$(dom_dev $n) || { log "DOM synoboot$n not found: not locked"; continue; }
        ! grep -q "^$dev $DOM_LOCK " /proc/mounts || continue
        fs=$(dom_fs "$dev") || { log "DOM $dev: unknown file system, not locked"; continue; }
        mkdir -p $DOM_LOCK
        if mount -t "$fs" -o ro "$dev" $DOM_LOCK 2>/dev/null; then
            log "DOM $dev locked (read-only on $DOM_LOCK)"
        else
            log "could not mount $dev on $DOM_LOCK: the DOM is not locked in the partitioner"
        fi
    done
}
dom_unprotect() {
    i=0
    while grep -q " $DOM_LOCK " /proc/mounts; do
        umount $DOM_LOCK 2>/dev/null && continue
        i=$((i + 1)); [ $i -lt 5 ] || return 1
        sleep 1
    done
}

# Block devices under the new system: the devices of its mounts on /target and of the active
# swaps, then down through partitions (md0p1 -> md0) and slaves (dm -> md -> sd), one name
# per line. MOUNTS, SWAPS and SYSBLK can be pointed elsewhere (tests).
target_blockdevs() {
    queue=$( { awk -v t=/target '$2 == t || index($2, t "/") == 1 { print $1 }' "${MOUNTS:-/proc/mounts}"
               awk 'NR > 1 { print $1 }' "${SWAPS:-/proc/swaps}"; } |
             while read -r d; do case $d in /*) basename "$(readlink -f "$d")" ;; esac; done | tr "\n" " ")
    seen=" "
    while [ -n "$queue" ]; do
        n=${queue%% *}
        case $queue in *" "*) queue=${queue#* } ;; *) queue= ;; esac
        [ -n "$n" ] || continue
        case $seen in *" $n "*) continue ;; esac
        seen="$seen$n "
        p=${SYSBLK:-/sys/class/block}/$n
        [ -e "$p" ] || continue
        echo "$n"
        [ ! -e "$p/partition" ] || queue="$queue $(basename "$(dirname "$(readlink -f "$p")")")"
        for s in "$p"/slaves/*; do [ ! -e "$s" ] || queue="$queue ${s##*/}"; done
    done
}

# mdadm.conf body from "mdadm --detail --scan" ($1) and target_blockdevs ($2): the arrays
# the new system uses as ARRAY lines, the others (DSM's, assembled by the installer)
# commented out, so that "AUTO -all" keeps them stopped.
md_conf_arrays() {
    printf '%s\n' "$1" | while IFS= read -r line; do
        case $line in ARRAY\ *) ;; *) continue ;; esac
        dev=${line#ARRAY }; dev=${dev%% *}
        n=$(basename "$(readlink -f "$dev")")
        if printf '%s\n' "$2" | grep -qx "$n"; then
            printf '%s\n' "$line"
        else
            printf '# not used by this system: %s\n' "$line"
        fi
    done
}
