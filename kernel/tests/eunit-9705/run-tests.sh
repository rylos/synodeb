#!/bin/bash
# Userspace tests of the synology_eunit_9705 encoding, unit table and write guard
# (kernel/src/drivers/ata/synology_eunit_9705.h). The unmodified header must pass; every
# deliberately broken copy (mutant) must fail, which shows the test can fail.
# Also checks that the driver sends PMP writes from one place only (eu_write, behind the guard).
# Usage: kernel/tests/eunit-9705/run-tests.sh   (needs gcc, perl)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/../../src/drivers/ata" && pwd)
H=$SRC/synology_eunit_9705.h
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
rc=0

build_run() { # header -> 0 if the test passes
    gcc -O2 -Wall -Wextra -Werror -DEU_HEADER="\"$1\"" -o "$W/t" "$HERE/test-eunit-9705.c" &&
        "$W/t" > "$W/out" 2>&1
}

if build_run "$H"; then
    echo "ok: unmodified header passes ($(tail -1 "$W/out"))"
else
    echo "FAIL: unmodified header"; cat "$W/out"; rc=1
fi

mut() { # name, perl substitution on a copy of the header: the test must fail
    local m=$W/$1.h
    perl -0pe "$2" "$H" > "$m"
    if cmp -s "$H" "$m"; then echo "FAIL: mutant $1 changed nothing"; rc=1; return; fi
    if build_run "$m"; then echo "FAIL: mutant $1 passed"; rc=1
    else echo "ok: mutant $1 caught"; fi
}

mut stop_allowed   's/ &&\n\t\t       \(d & EU_P4_FAN\) != EU_FAN_STOP;/;/'
mut pulse_allowed  's/!\(\(d \| g->cur\) & EU_P4_POWER_PULSE\) &&//'
mut not_master     's/EU_P0_EMID\(g->page\[0\]\) == 0 &&//'
mut p4_any_bits    's/!\(\(d \^ g->cur\) & ~EU_P4_FAN\) &&//'
mut buzzer_allowed 's/!\(\(d \| g->cur\) & EU_P1_BUZZER\) &&//'
mut p1_any_bits    's/ &&\n\t       !\(\(d \^ g->cur\) & ~g->p1_mask\);/;/'
mut no_id_check    's/ \|\| EU_P0_ID\(g->page\[0\]\) != g->id\)/)/'
mut no_p0_check    's/!\(g->valid & 1U\) \|\| //'
mut any_byte       's/d != g->new \|\| //'
mut any_word       's/ \|\| \(val != EU_R_DATA\(p, d, 0\) && val != EU_R_DATA\(p, d, 1\)\)//'
mut unknown_writes 's/!g->writable \|\| //'
mut not_armed      's/\(p != 1 && p != 4\)/(p != 1 \&\& p != 4 \&\& p != -1)/'
mut pol_any        's/return val == EU_POL_ALL && eu_id_ok\(g\);/return true;/'
mut pol_before_id  's/return val == EU_POL_ALL && eu_id_ok\(g\);/return val == EU_POL_ALL;/'
mut id_no_table    's/!eu_type_lookup\(EU_P0_ID\(g->page\[0\]\)\)/0/'
mut id_any_row     's/return !g->id \|\| EU_P0_ID\(g->page\[0\]\) == g->id;/return true;/'
mut poweron_writes 's/\tif \(g->poweron && !g->setup_ok\)\n\t\treturn false;\n//'
mut restore_any    's/\t\t\treturn g->poweron;\n/\t\t\treturn true;\n/'
mut inputs_any     's/return g->poweron && \(val == EU_R_IDLE \|\| val == 0\);/return EU_R_SELECT_OK(val) || val == 0;/'
mut no_invert      's/return EU_DEC\(pol == EU_POL_ALL \? r : r \^ EU_POL_ALL\);/return EU_DEC(pol == EU_POL_ALL ? r : r);/'
mut pol_any_known  's/return pol == EU_POL_ALL \|\| pol == 0;/return pol == EU_POL_ALL || pol < 2;/'
mut unconfirmed_rw 's/return t && \(t->confirmed \|\| unconfirmed\);/return t \&\& (t->confirmed || unconfirmed || 1);/'
mut led_no_skip    's/return \(cur \^ want\) & want_mask & p1_mask & ~EU_P1_BUZZER;/return (cur | want | 0xff) \& want_mask \& p1_mask \& ~EU_P1_BUZZER;/'
mut en_any         's/return val == EU_EN_ALL_OUT \|\| val == EU_EN_READ;/return true;/'
mut other_reg      's/eu_data_ok\(g, val\);\n\t\}\n\treturn false;/eu_data_ok(g, val);\n\t}\n\treturn true;/'
mut select_loose   's/#define EU_R_SELECT_OK\(r\)\t\(\(\(r\) & \(EU_R_WCTL \| ~EU_R_ALLOWED\)\) == EU_R_WCTL\)/#define EU_R_SELECT_OK(r)\t(((r) \& ~EU_R_ALLOWED) == 0)/'
mut select_data    's/#define EU_R_ALLOWED\t\t\(EU_R_WCTL/#define EU_R_ALLOWED\t\t(0x1fU | EU_R_WCTL/'
mut wctl_bit       's/#define EU_R_WCTL\t\t\(1U << 14\)/#define EU_R_WCTL\t\t(1U << 13)/'
mut page_shift     's/\(\(\(p\) & 3U\) << 17\)/(((p) \& 3U) << 16)/'
mut p4_new_pulse   's/\(code & EU_P4_FAN\)\) & ~EU_P4_POWER_PULSE;/(code \& EU_P4_FAN));/'
mut p1_new_buzzer  's/return \(\(cur & ~m\) \| \(v & m\)\) & ~EU_P1_BUZZER;/return (cur \& ~m) | (v \& m);/; s/u8 m = mask & p1_mask & ~EU_P1_BUZZER;/u8 m = mask \& p1_mask;/'
mut row_bays       's/\.name = "DX517", \.bays = 5/.name = "DX517", .bays = 4/'
mut row_dx1222     's/\.led_bit = \{ 3, 2, 1 \}/.led_bit = { 0, 1, 2 }/'
mut row_confirmed  's/\.name = "DX517", (.*)\n(.*)\} \},/.name = "DX517", $1\n$2}, .confirmed = true },/'
mut row_led_bit5   's/\.name = "RX1217", (.*)\n\t  \.led_bit = \{ 0, 1, 2 \}/.name = "RX1217", $1\n\t  .led_bit = { 0, 1, 5 }/'
mut retry_any_err  's/return \(err == -EIO \|\| err == -EAGAIN\) && fails < EU_LED_TRIES;/return err \&\& fails < EU_LED_TRIES;/'
mut retry_forever  's/return \(err == -EIO \|\| err == -EAGAIN\) && fails < EU_LED_TRIES;/return (err == -EIO || err == -EAGAIN) \&\& (fails < EU_LED_TRIES || fails);/'
mut retry_tries    's/#define EU_LED_TRIES\t\t5/#define EU_LED_TRIES\t\t50/'
mut requeue_drop   's/return pending \| tried;/return pending | (tried \& 0);/'

# the driver: no LED round queued or run while suspended
set_body=$(awk '/^static int eu_led_set\(/,/^}/' "$SRC/synology_eunit_9705.c")
work_body=$(awk '/^static void eu_led_work_fn\(/,/^}/' "$SRC/synology_eunit_9705.c")
if grep -q 'if (!u->ctrl->suspended) {' <<<"$set_body" &&
   [ "$(grep -c schedule_delayed_work <<<"$set_body")" = 1 ] &&
   [ "$(grep -n 'if (c->suspended)' <<<"$work_body" | head -1 | cut -d: -f1)" -lt \
     "$(grep -n 'eu_cycle(' <<<"$work_body" | head -1 | cut -d: -f1)" ]; then
    echo "ok: LED rounds are neither queued nor run while suspended"
else
    echo "FAIL: LED rounds while suspended"; rc=1
fi

# the driver: one sata_pmp_vendor_write() call, inside eu_write(), after the guard
C=$SRC/synology_eunit_9705.c
n=$(grep -c 'sata_pmp_vendor_write(' "$C")
body=$(awk '/^static unsigned int eu_write\(/,/^}/' "$C")
if [ "$n" = 1 ] && grep -q 'sata_pmp_vendor_write(' <<<"$body" &&
   [ "$(grep -n 'eu_write_ok' <<<"$body" | head -1 | cut -d: -f1)" -lt \
     "$(grep -n 'sata_pmp_vendor_write' <<<"$body" | cut -d: -f1)" ]; then
    echo "ok: one PMP write call, in eu_write() after eu_write_ok()"
else
    echo "FAIL: PMP write calls outside eu_write() ($n)"; rc=1
fi
if grep -nE 'sata_pmp_write\(|sata_scr_write|ata_exec_internal' "$C"; then
    echo "FAIL: other ATA write paths in the driver"; rc=1
else
    echo "ok: no other ATA write path in the driver"
fi
exit $rc
