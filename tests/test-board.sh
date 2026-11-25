#!/bin/sh
# tests/test-board.sh — /usr/lib/synodeb/board against fake sysfs trees: models chosen by DMI
# name, by DMI placeholder plus PCI devices, and by PCI devices alone (no DMI).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=$ROOT/pkg/synodeb/usr/lib/synodeb/board
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export SYNODEB_TEST_BOARDS=$ROOT/pkg/synodeb/usr/share/synodeb/boards
fail=0
pci() { # tree slot vendor device
    mkdir -p "$1/bus/pci/devices/$2"
    echo "0x$3" > "$1/bus/pci/devices/$2/vendor"
    echo "0x$4" > "$1/bus/pci/devices/$2/device"
}
dmi() { mkdir -p "$1/class/dmi/id"; echo "$2" > "$1/class/dmi/id/product_name"; }
check() { # name tree expected-model
    got=$(SYNODEB_TEST_SYS=$2 sh "$B" MODEL 2>/dev/null) || got=none
    if [ "$got" = "$3" ]; then echo "ok   $1: $got"; else echo "FAIL $1: $got, expected $3"; fail=1; fi
}
# DS920+ by its DMI name
dmi "$T/ds920" "DS920+"; pci "$T/ds920" 0000:00:00.0 8086 31f0
check "DMI name" "$T/ds920" "DS920+"
# DS415+: placeholder DMI name, Avoton host bridge and SiI3132
dmi "$T/ds415" MohonPeak; pci "$T/ds415" 0000:00:00.0 8086 1f0c; pci "$T/ds415" 0000:01:00.0 1095 3132
check "MohonPeak + PCI" "$T/ds415" "DS415+"
# DS415+ without DMI (boot loader that does not pass the EFI tables)
pci "$T/nodmi" 0000:00:00.0 8086 1f0c; pci "$T/nodmi" 0000:01:00.0 1095 3132
check "no DMI, PCI only" "$T/nodmi" "DS415+"

# DS2415+: MohonPeak with Marvell controllers, no SiI3132: not a DS415+, and no board file
dmi "$T/ds2415" MohonPeak; pci "$T/ds2415" 0000:00:00.0 8086 1f0c; pci "$T/ds2415" 0000:01:00.0 1b4b 9235
check "MohonPeak without SiI3132" "$T/ds2415" none
# no DMI and only one of the PCI devices: not a DS415+
pci "$T/half" 0000:01:00.0 1095 3132
check "no DMI, SiI3132 only" "$T/half" none
# any other machine (QEMU): no board file, not the DS923+ one
dmi "$T/qemu" "Standard PC (Q35 + ICH9, 2009)"; pci "$T/qemu" 0000:00:00.0 8086 29c0
check "unknown DMI name" "$T/qemu" none
exit $fail
