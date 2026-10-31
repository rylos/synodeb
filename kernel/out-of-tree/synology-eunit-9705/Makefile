# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build of synology_eunit_9705, for any kernel that already has the PMP
# vendor-register API (CONFIG_SATA_PMP_VENDOR_EH, kernel/patches/<x.y>, patch
# ata-libata-pmp-vendor-registers-in-a-quiet-eh-round); without it the build stops with
# an #error. The sources are kernel/src/drivers/ata/synology_eunit_9705.[ch], copied here.
#   make                      build for the running kernel
#   make KDIR=/path/to/tree   build for another kernel tree
#   make sources              copy the sources only (before "dkms add .")
KDIR ?= /lib/modules/$(shell uname -r)/build
HERE := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
UPSTREAM := $(HERE)/../../src/drivers/ata
FILES := synology_eunit_9705.c synology_eunit_9705.h

all: modules

sources:
	@if [ -d "$(UPSTREAM)" ]; then \
		for f in $(FILES); do cmp -s "$(UPSTREAM)/$$f" "$(HERE)/$$f" || cp "$(UPSTREAM)/$$f" "$(HERE)/$$f"; done; \
	fi
	@for f in $(FILES); do [ -f "$(HERE)/$$f" ] || { echo "missing $$f" >&2; exit 1; }; done

modules: sources
	$(MAKE) -C "$(KDIR)" M="$(HERE)" modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(HERE)" clean

.PHONY: all sources modules clean
