# synodeb installer: helpers shared by early and finish (busybox sh in the installer)

DOM_P1=10EE-589C                               # synoboot1, FAT: EFI/boot/SynoBootLoader.conf
DOM_P2=45e5b07d-4783-4867-a369-f99c0cd1e610    # synoboot2, ext2: kernel + initrd
DOM_MNT=/run/synodeb-dom
DOM_LOCK=/hd-media
LOG=/var/log/synodeb-installer.log

log() { echo "synodeb: $*" >> $LOG; logger -t synodeb -- "$*" 2>/dev/null || true; }

# --- front-panel microcontroller (uP): UART 8250 at MMIO 0xFEDCA000, 9600 8N1 ---
up_dev() {
    for t in /sys/class/tty/ttyS*; do
        b=$(tr a-f A-F < "$t/iomem_base" 2>/dev/null)
        [ "$b" = 0XFEDCA000 ] && { echo "/dev/${t##*/}"; return 0; }
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
dom_mount() { # uuid fstype
    dev=$(blkid -U "$1" 2>/dev/null) || return 1
    mkdir -p $DOM_MNT
    dom_modules
    mount -t "$2" "$dev" $DOM_MNT 2>/dev/null
}
dom_umount() { sync; umount $DOM_MNT 2>/dev/null; }
# make the menu entry titled $1 the default of the DOM GRUB
dom_default() {
    conf=$DOM_MNT/EFI/boot/SynoBootLoader.conf
    dom_mount $DOM_P1 vfat || { log "DOM synoboot1 not found"; return 1; }
    # entries counted as synodeb-setup.sh and synodeb-dom do: lines starting with menuentry
    n=0; idx=
    while IFS= read -r l; do
        l=${l#"${l%%[! 	]*}"}
        case "$l" in
        "menuentry \"$1\""*) idx=$n; break ;;
        "menuentry "*) n=$((n + 1)) ;;
        esac
    done < "$conf"
    rc=1
    if [ -n "$idx" ] && sed "s/^set default='[0-9]*'/set default='$idx'/" "$conf" > "$conf.tmp" &&
       grep -q "^set default='$idx'" "$conf.tmp" && mv -f "$conf.tmp" "$conf"; then
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
    for u in $DOM_P1:vfat $DOM_P2:ext2; do
        dev=$(blkid -U "${u%:*}" 2>/dev/null) || { log "DOM ${u%:*} not found: not locked"; continue; }
        ! grep -q "^$dev $DOM_LOCK " /proc/mounts || continue
        mkdir -p $DOM_LOCK
        if mount -t "${u#*:}" -o ro "$dev" $DOM_LOCK 2>/dev/null; then
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
