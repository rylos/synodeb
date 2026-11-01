#!/bin/sh
# synodeb board mapping helper. Read-only: it reads /sys/kernel/debug/gpio and the first bytes of
# the DOM partitions, and writes nothing to the hardware. Run as root under DSM (busybox sh) or Linux, on an AMD Ryzen Synology (pinctrl
# AMDI0030).
#
# It finds the bay presence and bay power pins, which differ between models, by reading the
# GPIO states while you hot-plug one disk, and the fan tachometer pins, which toggle by
# themselves. It also prints the DOM partition identity (FAT volume id, ext2 UUID, sizes)
# that synodeb-setup.sh checks before it touches the DOM. Under DSM, hot-plugging a disk into an empty bay is safe: DSM only lists the new
# disk, it does not touch it until you create something on it. Use a disk without data you need.
# Only use empty bays: skip a bay that holds a disk, and never remove a disk that belongs to a
# storage pool (the pool would become degraded).
#
# Output: ./synodeb-board-<date>.txt  (attach it to your hardware report)
umask 077
out="./synodeb-board-$(date +%Y%m%d-%H%M).txt"
if [ -e "$out" ] || [ -L "$out" ]; then echo "$out already exists, not overwriting it" >&2; exit 1; fi
src=${GPIO_SRC:-/sys/kernel/debug/gpio}
[ -r "$src" ] || { echo "cannot read $src (run as root; debugfs mounted?)" >&2; exit 1; }

# one line per pin: "pinN in high", "pinN out low", ... (bank0, pins 0-183)
snap() {
    awk '
        function emit(   d, v) {
            if (p == "") return
            d = r ~ /output is enabled/ ? "out" : "in"
            v = "?"
            if (d == "out" && match(r, /output is (high|low)/)) v = substr(r, RSTART + 10, RLENGTH - 10)
            if (d == "in" && match(r, /input is (high|low)/)) v = substr(r, RSTART + 9, RLENGTH - 9)
            print p, d, v
        }
        /^GPIOs/ { next }
        { sub(/^GPIO bank[0-9]+[ \t]+/, "") }
        /^pin[0-9]+/ { emit(); p = $1; r = $0; next }
        { r = r " " $0 }
        END { emit() }
    ' "$src"
}

diffsnap() {   # $1 $2: snapshots; prints the pins that changed
    printf '%s\n' "$1" > "$tmpd/a"; printf '%s\n' "$2" > "$tmpd/b"
    awk 'NR == FNR { s[$1] = $2 " " $3; next } ($1 in s) && s[$1] != $2 " " $3 { print "  " $1 ": " s[$1] " -> " $2 " " $3 }' "$tmpd/a" "$tmpd/b"
}

tmpd=$(mktemp -d ./synodeb-board.XXXXXX) || exit 1
trap 'rm -rf "$tmpd"' EXIT
log() { printf '%s\n' "$*" | tee -a "$out"; }
ask() { printf '\n%s\nPress Enter when done (or type s and Enter to skip): ' "$*"; read -r a; [ "$a" != s ]; }
echo "Only empty bays are used. Skip (s) every step for a bay that already holds a disk:"
echo "never pull a disk that belongs to a storage pool."

log "===== synodeb board map $(date -u +%FT%TZ)"
log "model: $(cat /proc/sys/kernel/syno_hw_version 2>/dev/null || cat /sys/class/dmi/id/product_name 2>/dev/null)"
# DOM identity: the same bytes synodeb-setup.sh checks (FAT16 volume id at 39, or FAT32 at 67;
# ext2 magic at 1080 and UUID at 1128), and the partition sizes in 512-byte sectors
bytes() { dd if="$1" bs=1 skip="$2" count="$3" 2>/dev/null | od -An -tx1 | tr -s ' \n' ' ' | sed 's/^ //; s/ $//'; }
log "===== DOM"
for p in synoboot1 synoboot2; do
    if [ -b "/dev/$p" ]; then
        log "$p: sectors $(cat /sys/class/block/$p/size 2>/dev/null) fat16-id [$(bytes /dev/$p 39 4)] fat32-id [$(bytes /dev/$p 67 4)] ext2-magic [$(bytes /dev/$p 1080 2)] ext2-uuid [$(bytes /dev/$p 1128 16)]"
    else
        log "$p: no /dev/$p"
    fi
done

base=$(snap)
log "===== baseline (all pins)"
printf '%s\n' "$base" >> "$out"

# fan tachometers: inputs that change by themselves while nothing else happens
log "===== pins toggling by themselves (fan tachometers?), 200 reads"
: > "$tmpd/t"
i=0
while [ $i -lt 200 ]; do snap >> "$tmpd/t"; i=$((i + 1)); done
awk '{ seen[$1 " " $3] = 1; pins[$1] = 1 } END { for (p in pins) if ((p " high") in seen && (p " low") in seen) print substr(p, 4) }' "$tmpd/t" |
    sort -n | sed 's/^/  pin/' | tee -a "$out"

prev=$base
for step in \
    "Insert a disk into bay 3 (skip if bay 3 holds a disk). Wait until its activity LED lights up or DSM lists it (about 30 s)." \
    "Remove the disk from bay 3. Wait 15 s." \
    "Insert the disk into bay 4 (skip if bay 4 holds a disk). Wait about 30 s." \
    "Remove the disk from bay 4. Wait 15 s." \
    "Insert the disk into bay 5 (skip if bay 5 holds a disk). Wait about 30 s." \
    "Remove the disk from bay 5. Wait 15 s."
do
    if ask "$step"; then
        now=$(snap)
        log "===== $step"
        diffsnap "$prev" "$now" | tee -a "$out"
        prev=$now
    else
        log "===== skipped: $step"
    fi
done

log "===== end"
echo "wrote $out. Please read it before attaching."
