# Platform geminilake (Intel Celeron J4025/J4125, Gemini Lake)

Models: DS920+ (supported). Board file: `boards/ds920plus.conf`.

Common hardware (DS920+, measured under DSM 7.4.1): front-panel microcontroller on LPSS UART 0
(PCI 00:18.0, 9600 8N1, the same protocol as r1000), serial console on LPSS UART 2 (PCI 00:18.2);
Seiko S-35390A RTC on LPSS I2C 0 (ACPI RTC35390, address 0x30); disk LEDs on a TI LP3943 on the
SMBus (ACPI LED3943, address 0x60); bay power, bay presence, USB VBUS and fan-fail inputs on the
Gemini Lake GPIO community "northwest" (INT3453 UID 1); SATA on a Marvell 88SE9235 (bays 1-2,
eSATA) and on the SoC AHCI (bays 3-4); M.2 NVMe slots on PCIe root ports 00:14.0 and 00:14.1.

The NVMe names are not stable: `nvme0n1` and `nvme1n1` can swap from one boot to the next
(DS920+, both seen). Use the PCI path to tell the slots apart: M.2 slot 1 is behind 00:14.1
(`/dev/disk/by-path/pci-0000:05:00.0-nvme-1`), slot 2 behind 00:14.0
(`/dev/disk/by-path/pci-0000:04:00.0-nvme-1`).

Kernel:
- `kernel/src/drivers/platform/x86/synology_nas.c` (shared, table `ds920plus`): bay power
  (GPIO 20-23) and spin-up groups 2,1,1, bay presence (35-38, active low), USB VBUS (29, 30),
  fan tachometers (39, 40), the disk LED enable line (17, high at probe), the RTC (the ACPI
  RTC35390 client is replaced by an s35390a one with the INT1 alarm), the microcontroller
  restart/power-off bytes on LPSS UART 0, and the MAC addresses of both ports (the NICs come up
  with a Realtek default address; the right ones are passed by the DOM boot loader in `macs=`)
- `kernel/src` here: `drivers/leds/leds-synology-lp3943.c`, the disk LEDs (ACPI LED3943),
  Kconfig in `kernel/patches/<x.y>/*-leds-synology-lp3943.patch`

Fully tested on a DS920+ with the 7.3 kernel: installation over SSH, boot from the DOM, network,
bay power and presence, RTC, fan speed and the DS920+ fan curves, CPU and disk temperatures,
power LED, disk LEDs, power-off, power and reset buttons, a btrfs RAID1 root on both NVMe
drives, kernel updates from the apt repository with the kernel trial, hardware video (iGPU). The microcontroller reports the power
button only when it is held for about 3 seconds (as under DSM), the reset button on a short
press; its id reply to `R` is not framed (`P...`, read by synohwd).

The Realtek NICs ask for `rtl_nic/rtl8168h-2.fw` and work without it; the installer adds
`firmware-realtek` (Debian non-free-firmware) when the kernel asked for it during the
installation, on an existing system: `apt install firmware-realtek`.

iGPU (Intel UHD 600, PCI 00:02.0, device 3185): i915 as a module, `/dev/dri/renderD128`.
With `firmware-misc-nonfree` (the installer adds it) the DMC, GuC 70.1.1 and HuC 4.0.0
firmware load; `enable_guc=2` (modprobe.d/synodeb.conf) makes HuC authenticate, which the
low-power H.264 encoder needs. Measured on a DS920+ with 7.3 and iHD 25.2.3: 1080p H.264
decode + low-power encode at about 140 fps, 1080p to 720p with VPP scaling about 200 fps,
HEVC encode about 40 fps (non-free driver); `intel_gpu_top` works.

Not done yet: the DX517 on the eSATA port (the expansion-unit driver matches DMI vendor
"Synology Inc.", this BIOS says "Synology").
