# On the installed system

## Commands

They are in `/usr/sbin`, which is not in a normal user's `PATH`: run them with `sudo`
(`synodeb-dom` needs root, it mounts the DOM).

| command | |
|---|---|
| `sudo synohwd status` | fan duty and speeds, temperatures, bays, recent events |
| `sudo synodeb-dom status` | boot entries, their command line and the kernels on the DOM |
| `sudo synodeb-dom default debian\|debian-old\|dsm` | default boot entry (then reboot) |
| `sudo synodeb-dom setup` | rewrite the boot entries (after changing `/etc/default/synodeb`, or when `status` says they are outdated) |
| `sudo synodeb-ups status` | UPS and Safe Mode |
| `sudo synodeb-dsm` | mount DSM volumes read-only (`synodeb-dsm --help`) |

## Switching between Debian and DSM

| from | to | command |
|---|---|---|
| Debian | DSM | `sudo synodeb-dom default dsm && sudo reboot` |
| DSM | Debian | `sudo bash synodeb-setup.sh boot debian && sudo reboot` |
| Debian | Debian, previous kernel | `sudo synodeb-dom default debian-old && sudo reboot` |

The choice stays until it is changed again.

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
- `/etc/synodeb/notify.conf`: notifications by e-mail (SMTP), Telegram or ntfy, for SMART,
  RAID, fans, UPS, failed services and unclean shutdowns. Nothing is sent until a channel is
  configured.
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
  (`SAFESHUTDOWN`).

## DSM data

`synodeb-dsm` assembles and mounts DSM volumes read-only (mdadm, LVM, btrfs with the Synology
extensions), e.g. to copy the data to the new Debian volumes.

## Updates

Debian's own security updates are installed automatically. The kernel and the `synodeb`
package come from the synodeb apt repository and are updated with
`sudo apt update && sudo apt full-upgrade`, then a reboot. See [kernel](kernel.md).
