# Platform geminilake (Intel Celeron J4025/J4125, Gemini Lake)

Models: DS920+ (work in progress). Board file: `boards/ds920plus.conf`.

Common hardware (DS920+, measured under DSM 7.4.1): front-panel microcontroller on LPSS UART 0
(PCI 00:18.0, 9600 8N1, the same protocol as r1000), serial console on LPSS UART 2 (PCI 00:18.2);
Seiko S-35390A RTC on LPSS I2C 0 (ACPI RTC35390, address 0x30); disk LEDs on a TI LP3943 on the
SMBus (ACPI LED3943, address 0x60); bay power, bay presence, USB VBUS and fan-fail inputs on the
Gemini Lake GPIO community "northwest" (INT3453 UID 1); SATA on a Marvell 88SE9235 (bays 1-2,
eSATA) and on the SoC AHCI (bays 3-4).

Kernel support: to be added (`kernel/src` here).
