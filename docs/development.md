# How it works and how to build it

## How it works

The firmware of these NAS always boots the **DOM**, a 128 MB USB flash module on the mainboard
that holds the Synology boot loader (GRUB) and the DSM kernel. synodeb adds two entries to its
boot menu, *DEBIAN* and *DEBIAN_OLD*, and copies the Debian kernel and initrd next to the DSM
ones. Everything else, the whole Debian system, is on the disks you choose.

The kernel (see [kernel](kernel.md)) adds to the kernel.org release:

- a platform driver for bay power and presence, USB power, fan speed sensors, the RTC alarm on
  the power-on line and the power-off and reboot sequence of the front-panel microcontroller,
  with one table per model;
- the disk LED drivers (ATmega1608 on the DS923+, TI LP3943 on the DS920+) and per-port ATA LED
  triggers;
- SATA PHY tuning (JMB585 on the DS923+);
- read-only mounting of btrfs volumes written by DSM;
- longterm only: md superblocks written by newer kernels are accepted.

`synohwd` (Python) talks to the front-panel microcontroller: fans, LEDs, buttons, beeper,
power-on settings.

The installer is the official Debian netboot installer with the kernel modules replaced by those
of the synodeb kernel, a preseed for the steps that run before SSH (language, network, mirror)
and a final step that installs the synodeb kernel and package and writes the DOM.

## Models

Everything specific to one model is in its board file, `boards/<model>.conf`, and in its
platform directory, `platforms/<platform>/` (hardware notes and kernel sources):

| platform | SoC | models |
|---|---|---|
| [r1000](../platforms/r1000/README.md) | AMD Ryzen Embedded R1000 | DS923+, DS1522+ (work in progress) |
| [geminilake](../platforms/geminilake/README.md) | Intel Celeron J4125 | DS920+ |

`tools/sync-boards.py` copies the board files into `synodeb-setup.sh` (a single downloadable
file) and into the package. The plan for more models is in
[#7](https://github.com/rylos/synodeb/discussions/7).

## Building

Everything builds in `debian:trixie` containers (Docker).

```sh
kernel/run-build.sh                 # kernel .deb packages, longterm series (kernel/longterm/VERSION)
SERIES=stable kernel/run-build.sh   # stable series (kernel/stable/VERSION)
installer/run-build.sh              # build/out/installer: vmlinuz, installer.img (longterm kernel)
tools/publish.sh                    # apt repository and installer for GitHub Pages (build/pages);
                                    # SERIES=stable publishes the stable kernel packages
tests/run-test-setup.sh DOM.img     # synodeb-setup.sh against a copy of a DOM image
tools/build-backport.sh nut         # build/out/backports: a sid package rebuilt for trixie
```

The apt repository also carries packages rebuilt from Debian sid when trixie's version lacks
something the NAS needs: NUT 2.8.5 (`2.8.5-1~synodeb13+1`), whose drivers know UPS models that
2.8.1 does not. They keep trixie's configuration files in `/etc/nut`, so the upgrade asks
nothing, and a newer Debian package replaces them. `tools/publish.sh` adds what is in
`build/out/backports`; later runs, CI included, keep it.

Each series has its `kernel/<series>/VERSION` (upstream version and ABI); patches and Debian
configuration are per branch, in `kernel/patches/<x.y>` and `kernel/config-debian-<x.y>`.
`tools/publish.sh` builds the packages in `pkg/` (`synodeb`, and `synology-ds923`, its
transitional package) from the committed tree (`git archive HEAD`): a change to a package is
published only with a new `Version` in its `DEBIAN/control`, and a version already in the
repository with a different content is refused.

## CI

The workflow [`kernel`](../.github/workflows/kernel.yml) checks kernel.org every day for both
series: when a branch has a new release it bumps the version and ABI, builds the kernel, adds it
to the signed apt repository and commits the new `VERSION`. A newer branch (a new longterm, or
the next stable x.y) only gives a warning: moving to it means porting the patches. It can also
be started by hand (one series or both, rebuild with a new ABI, rebuild the installer, or build
only).

The workflow [`test`](../.github/workflows/test.yml) runs on every push and pull request: the
synohwd tests, the expansion-unit driver tests, a check that the packaged `synohwd` is the same
as `synohwd/synohwd.py`, a check that the board files match their copies
(`tools/sync-boards.py --check`), and shellcheck on the shell scripts.
