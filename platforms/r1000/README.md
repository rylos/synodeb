# Platform r1000 (AMD Ryzen Embedded R1000)

Models: DS923+ (supported), DS1522+ (work in progress). Board files: `boards/ds923plus.conf`,
`boards/ds1522plus.conf`.

Common hardware: AMD Ryzen R1600; front-panel microcontroller on the AMD UART at MMIO
0xFEDCA000 (9600 8N1), serial console at 0xFEDC9000; Seiko S-35390A RTC on the AMD I2C bus;
bay power, bay presence and USB VBUS on AMD GPIOs (AMDI0030); SATA on a JMB585 behind 00:01.5
and on the FCH behind 00:08.2.

Kernel (`kernel/src` here, overlaid on the shared kernel tree; Kconfig in
`kernel/patches/<x.y>/0001-platform-x86-synology-ds923.patch`):
- `drivers/platform/x86/synology_ds923.c`: bay power and spin-up groups, bay presence, USB
  VBUS, fan tachometers, RTC and LED controller instantiation; one table per model (DMI)
- `drivers/leds/leds-atmega1608.c`: the disk LED controller of the DS923+

Patch `0003-ahci-synology-ds923-jmb585-phy` holds the JMB585 PHY tuning of each model.
