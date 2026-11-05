# On the installed system

## Commands

They are in `/usr/sbin`, which is not in a normal user's `PATH`: run them with `sudo`
(`synodeb-dom` needs root, it mounts the DOM).

| command | |
|---|---|
| `sudo synohwd status` | fan duty and speeds, temperatures, bays, recent events |
| `sudo synodeb-dom status` | boot entries, their command line and the kernels on the DOM (`--json`: the same as one JSON object, for other tools) |
| `sudo synodeb-dom default debian\|debian-old\|dsm` | default boot entry (then reboot) |
| `sudo synodeb-dom sync --root DIR` | copy the kernels of a root snapshot mounted on DIR to the DOM, before booting it ([kernel](kernel.md#rolling-the-root-back-to-a-snapshot)) |
| `sudo synodeb-dom setup` | rewrite the boot entries (after changing `/etc/default/synodeb`, or when `status` says they are outdated) |
| `sudo synodeb-ups status` | UPS and Safe Mode |
| `sudo synodeb-ups test quick` | UPS battery self-test (`deep`, `stop`, `result`) |
| `sudo synodeb-net status` | onboard network ports: MAC, name, link |
| `sudo synodeb-dsm` | mount DSM volumes read-only (`synodeb-dsm --help`) |

## Switching between Debian and DSM

| from | to | command |
|---|---|---|
| Debian | DSM | `sudo synodeb-dom default dsm && sudo reboot` |
| DSM | Debian | `sudo bash synodeb-setup.sh boot debian && sudo reboot` |
| Debian | Debian, previous kernel | `sudo synodeb-dom default debian-old && sudo reboot` |

The choice stays until it is changed again.

## Network port names

The onboard ports are called `lan1`, `lan2`... as labelled on the back of the NAS, whatever
the kernel or systemd version (one systemd `.link` file per port in `/etc/systemd/network`,
matched on its MAC address, as Proxmox VE does). The installer sets this up; the systemd name
(e.g. `enp3s0`) stays as an alternative name. On a system installed before, run
`sudo synodeb-net pin` (it also renames the ports in `/etc/network/interfaces`, keeping a
`.synodeb-net` copy) and reboot. Wake-on-LAN (`wol` in `/etc/synohwd.toml`) applies to the
`lan*` ports. USB adapters keep their `enx<MAC>` names, which are stable already.

## Hardware video (Intel iGPU)

On models with an Intel iGPU (DS920+) the kernel loads i915 and the GPU is
`/dev/dri/renderD128`. For VA-API (Jellyfin, Immich, Frigate, ffmpeg):

- firmware: `firmware-misc-nonfree` (installed by the installer; on an older system
  `sudo apt install firmware-misc-nonfree`, then reboot);
- access: the render node belongs to the group `render`. The installer adds its user to it;
  other users: `sudo usermod -aG render <user>`. Containers: `--device /dev/dri` and
  `--group-add` with the GID of `render` (`getent group render`);
- driver on the host, if ffmpeg runs there: `intel-media-va-driver` (H.264 encoding, decoding),
  or `intel-media-va-driver-non-free` from Debian's `non-free` component for VPP scaling and
  HEVC encoding. Jellyfin's own ffmpeg brings its driver;
- check: `vainfo --display drm --device /dev/dri/renderD128`, `intel_gpu_top`
  (package `intel-gpu-tools`).

Low-power H.264 encoding needs the HuC firmware, loaded through `options i915 enable_guc=2`
in `/etc/modprobe.d/synodeb.conf` (the kernel logs it as a "dangerous option" and marks itself
tainted; it changes nothing else).

## Emergency mode

If the boot stops in emergency or rescue mode (typically a filesystem in `/etc/fstab` without
`nofail` whose disk is missing, or a failed fsck), the NAS still brings up its network (as in
`/etc/network/interfaces`, DHCP on the wired ports if that gives no address) and an ssh
server, and sends an urgent notification with the cause and the address. Log in as usual,
look at `sudo journalctl -xb`, fix the cause, then `sudo systemctl default` (or reboot).
`systemctl` needs `sudo` there (D-Bus is not running).

With the root account locked, the default when the installer set up `sudo`, systemd retries
the normal boot every few minutes on its own; the ssh session stays up meanwhile. With a root
password the NAS reboots by itself after 10 minutes, postponed while someone is logged in
over ssh (`sudo systemctl stop 'synodeb-emergency-reboot*.timer'` to stay).

## Configuration

- `/etc/synohwd.toml`: fan mode (the DSM quiet or cool curve, or full speed) and minimum fan
  duty, beeper, disk LEDs, power on after AC loss, Wake-on-LAN. The curves themselves are
  fixed. The fans go to full speed while a fan has failed, or when the CPU, an NVMe drive or a
  disk that had a temperature sensor gives no reading.
  Other programs can read `/run/synohwd/status.json`, or use the control socket
  `/run/synohwd/control.sock` (root only, one JSON object per line: `ping`, `status`, `send`
  with the reply or the reason of a refusal; see `synohwd --help`).
- `/etc/synodeb/notify.conf`: notifications by e-mail (SMTP), Telegram or ntfy, for SMART,
  RAID, fans, UPS, failed services and unclean shutdowns. Nothing is sent until a channel is
  configured. While the rynas NAS manager runs (`/run/rynas/notify.sock`),
  every notification goes to it instead, with all priorities, and it applies its own rules;
  `FORWARD=off` in `notify.conf` keeps them on synodeb's channels.
- `/etc/synodeb/ups.conf`: UPS behaviour, see below.
- `/etc/default/synodeb`: extra kernel parameters (run `sudo synodeb-dom setup` after a change).

## UPS

A USB UPS works like on DSM: when its battery runs low the NAS enters *Safe Mode* (services
stopped, volumes unmounted, disks in standby) instead of powering off, and reboots by itself
when mains power is back.

- **Connected during the installation**: nothing to do, NUT is set up and enabled.
- **Connected later**: plug it in and run `sudo synodeb-ups setup --keep-password`.
- **Check**: `sudo synodeb-ups status` (or `upsc ups@localhost`).
- **Other machines using this UPS** (Home Assistant, another NAS, a PC with NUT): put their
  addresses in `/etc/synodeb/ups.conf`, set the password of the NUT user `monuser` and apply:

  ```sh
  sudo nano /etc/synodeb/ups.conf          # NETWORK_CLIENTS="192.0.2.10 192.0.2.11"
  sudo synodeb-ups passwd                  # password of monuser (e.g. the one DSM used)
  sudo synodeb-ups setup --keep-password
  sudo systemctl restart synodeb-nut-firewall nut-server
  ```

  The clients connect to port 3493 with UPS name `ups`, user `monuser` and that password; only
  the listed addresses are allowed. A client set up for DSM keeps working once the password is
  the same.
- Other options in the same file: dry run (`DRYRUN=1`, Safe Mode only logged), what to do when
  the UPS stops answering (`NOCOMM_SAFEMODE`), powering the UPS off at the end
  (`SAFESHUTDOWN`), Safe Mode after a time on battery instead of only when the battery runs low
  (`SAFEMODE_AFTER=600`, seconds, like DSM's "after N minutes").
- **Before Safe Mode**: executables in `/etc/synodeb/ups-pre-safemode.d` run first, in name
  order, all within `PRE_SAFEMODE_TIMEOUT` seconds (default 120), e.g. to stop VMs and
  containers cleanly. See the README there.
- **Battery test**: `sudo synodeb-ups test quick` (or `deep`, `stop`; `test result` shows the
  last result) asks the UPS for its self-test, if it supports it, as the NUT user `admin`.
  That user exists only for this, may run nothing but the battery test commands and has a
  random password that `setup` keeps (the upgrade to synodeb 4.3 adds it to a configuration
  written before). While a test started this way runs, the UPS
  going on battery is not notified and `SAFEMODE_AFTER` waits for the end of the test; low
  battery still means Safe Mode, so a deep test, which discharges the battery, is better run
  only with the NAS attended.

## DSM data

`synodeb-dsm` assembles and mounts DSM volumes read-only (mdadm, LVM, btrfs with the Synology
extensions), e.g. to copy the data to the new Debian volumes.

## Updates

Debian's own security updates are installed automatically. The kernel and the `synodeb`
package come from the synodeb apt repository and are updated with
`sudo apt update && sudo apt full-upgrade`, then a reboot. See [kernel](kernel.md).
