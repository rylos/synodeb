#!/bin/sh
# synodeb hardware report collector. Read-only: it only reads files and runs listing tools;
# it writes nothing to hardware and sends nothing anywhere. Run as root for full output.
# Written for synodeb, other Linux and DSM (busybox sh). Missing tools are skipped.
# Output: ./synodeb-report-<date>.txt  (review it, then attach it to the issue)
# Privacy: the host name, MAC addresses, serial numbers (incl. sn=/mac=/macs= on a DSM kernel
# command line), IPv4 addresses and UUIDs are replaced before the file is written. Best effort:
# read the file before attaching it. Disk models and firmware versions stay (they matter).
umask 077
out="./synodeb-report-$(date +%Y%m%d-%H%M).txt"
if [ -e "$out" ] || [ -L "$out" ]; then echo "$out already exists, not overwriting it" >&2; exit 1; fi
tmp=$(mktemp ./synodeb-report.XXXXXX) || exit 1
trap 'rm -f "$tmp"' EXIT
host=$(hostname 2>/dev/null || cat /proc/sys/kernel/hostname 2>/dev/null)
sec() { printf '\n===== %s\n' "$*"; }
run() { sec "$*"; sh -c "$*" 2>&1 | head -n 4000; }
have() { command -v "$1" >/dev/null 2>&1; }
{
sec "synodeb hardware report $(date -u +%FT%TZ)"
run "uname -a"
run "cat /etc/os-release /etc.defaults/VERSION 2>/dev/null"
run "cat /sys/class/dmi/id/sys_vendor /sys/class/dmi/id/product_name /sys/class/dmi/id/board_name /sys/class/dmi/id/bios_version 2>/dev/null"
run "cat /proc/sys/kernel/syno_hw_version 2>/dev/null"
# the kernel command line carries board facts: port maps, GPIO pins, console, disk counts
run "cat /proc/cmdline"
run "grep -m1 'model name' /proc/cpuinfo; grep -c ^processor /proc/cpuinfo; grep MemTotal /proc/meminfo"
# storage controllers, ports, port multipliers
have lspci && run "lspci -nn"
have lspci && run "lspci -nnk -d ::0106; lspci -nnk -d ::0104; lspci -nnk -d ::0107"
have lspci && run "lspci -tv"
run "dmesg | grep -iE 'ahci|ata[0-9]+(\.[0-9]+)?:|Port Multiplier|SATA link|pmp|sas|scsi host|Synology|eunit|ebox'"
run "for h in /sys/class/scsi_host/host*; do echo \"\$h -> \$(readlink -f \$h)\"; done"
run "for l in /sys/class/ata_link/*; do printf '%s ' \"\${l##*/}\"; cat \$l/sata_spd 2>/dev/null; done"
run "for f in /sys/class/scsi_host/host*/syno_pm_info; do [ -e \"\$f\" ] && { echo \"== \$f\"; cat \"\$f\"; }; done"
have lsscsi && run "lsscsi -g"
run "ls -l /dev/disk/by-path/ 2>/dev/null | grep -v -- -part"
have lsblk && run "lsblk -o NAME,MODEL,SIZE,TRAN,HCTL,TYPE 2>/dev/null"
# fans, temperatures, LEDs, GPIO, serial/USB control channels
run "for h in /sys/class/hwmon/hwmon*; do echo \"== \$h \$(cat \$h/name 2>/dev/null)\"; for f in \$h/*_label \$h/*_input \$h/pwm[0-9]* \$h/*_alarm; do [ -e \"\$f\" ] && echo \"\${f##*/} \$(cat \$f 2>/dev/null)\"; done; done"
have sensors && run "sensors"
run "ls /sys/class/leds/ 2>/dev/null"
run "cat /sys/kernel/debug/gpio 2>/dev/null | head -n 600"
run "ls -l /dev/ttyS* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null"
# plain lsusb prints the tree as well; DSM's lsusb is a Python script without -t, and the
# traceback it prints there says nothing about the hardware
have lsusb && run "lsusb"
have i2cdetect && run "i2cdetect -l"
run "cat /proc/interrupts | head -n 80"
# the whole kernel log of this boot (level 2 of the form). DSM keeps the journal in RAM with a
# small cap, so "journalctl -k -b" there often holds nothing from the boot; dmesg is collected
# as well. If the dmesg ring has wrapped too, nothing in the report is from the boot, and the
# report says so, because then level 1 and level 2 cannot be answered from it.
# Kernel audit records (apparmor, seccomp) are left out of both: they name binaries and file
# paths from the reporter's own system and say nothing about the hardware.
have journalctl && run "journalctl -k -b --no-pager | grep -vE 'audit\(|apparmor='"
run "dmesg | grep -vE 'audit\(|apparmor='"
if dmesg 2>/dev/null | head -n 1 | grep -q '^\[ *0\.'; then boot=yes; else boot=no; fi
sec "boot start present in the dmesg section: $boot"
[ "$boot" = yes ] || echo "The ring buffer no longer holds the start of this boot. Please reboot and run this script again."
} > "$tmp"
# Mask MAC addresses and serial numbers before sharing. A serial reaches the report in three
# shapes: "sn=VALUE" on a DSM kernel command line, "Serial Number: VALUE" from a listing tool,
# and "SN: VALUE" from a DSM kernel ata message. A four-part firmware version is not an IPv4
# address, "JMB585 Header Version: 09.00.00.01": only the number right after "Version:" is kept,
# hidden from the IPv4 rule, and any address elsewhere on that line is still masked. The last
# rule takes the serial off the end of a DSM lsusb line, "(Manufacturer Product SERIAL)": a token
# of six or more letters, digits, "-" or "_" that contains a digit counts as a serial. So a
# product name such as RTL8153 goes too when no serial follows it; the USB ids stay on the line.
sed -E '
s/([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}/xx:xx:xx:xx:xx:xx/g
s/\b((sn|SN|mac[0-9]*|macs|MAC[0-9]*|vender|hw_serial)=)[^ ]+/\1REDACTED/g
s/([Ss][Ee][Rr][Ii][Aa][Ll][ _A-Za-z.]*[:=] *)[^ ,]+/\1REDACTED/g
s/\b([Ss][Nn]:)( *)[^ ,]+/\1\2REDACTED/g
s/([Vv]ersion: *[0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\b/\1@DOT@\2@DOT@\3@DOT@\4/g
s/\b([0-9]{1,3}\.){3}[0-9]{1,3}\b/x.x.x.x/g
s/@DOT@/./g
s/[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}/UUID/g
/^ *\|__.*[( ][0-9A-Za-z_-]*[0-9][0-9A-Za-z_-]*\)( hub)?$/ s/^( *\|__.*[( ])[0-9A-Za-z_-]{6,}\)( hub)?$/\1REDACTED)\2/
' "$tmp" > "$out"
# host name (full, then the part before the first dot), regex characters escaped
for n in "$host" "${host%%.*}"; do
    if [ -n "$n" ] && [ "$n" != localhost ]; then
        h=$(printf %s "$n" | sed 's/[][\.*^$/]/\\&/g'); sed -i "s/\b$h\b/HOST/g" "$out"
    fi
done
echo "wrote $out ($(wc -l < "$out") lines). Please read it before attaching."
[ "$boot" = yes ] || echo "This report holds no boot log: the ring buffer has wrapped. Please reboot the unit and run the script again." >&2
