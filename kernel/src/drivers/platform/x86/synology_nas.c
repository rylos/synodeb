// SPDX-License-Identifier: GPL-2.0
/*
 * Synology NAS platform driver (DS923+, and the models in syno_dmi[]).
 *
 * The description below is that of the DS923+ (platform r1000). Other models
 * differ in the GPIO chip and pins, the bay count, the spin-up groups, the I2C
 * devices and the microcontroller UART: those are in a per-model table,
 * selected by DMI (struct syno_model). The DS920+ (platform geminilake) has
 * its GPIOs on INT3453:00 (bay power 20-23, presence 35-38, USB VBUS 29/30,
 * fan tachometers 39/40, disk LED enable 17), its RTC described in ACPI (RTC35390 on LPSS I2C 0)
 * and its microcontroller on LPSS UART 0 (PCI 00:18.0). Its Realtek NICs
 * carry no MAC address of their own: the Synology GRUB passes them on the
 * command line (macs=, from "vender" on the DOM), and DSM's order is kept.
 *
 * Implements, following the DSM 7.4 GPL kernel sources and the model.dtb of
 * the DS923+ (platform "r1000", board "Bilby"), the parts of the hardware DSM
 * drives itself:
 *
 *  - Front/rear USB VBUS: GPIO 42 (usb_slot@1, ports 1-2/2-2) and GPIO 40
 *    (usb_slot@2, ports 1-3/2-3), active high. The firmware never powers
 *    them, so this is done first, before anything else in probe.
 *  - SATA bay power: one AMD FCH GPIO per bay (4, 5, 8, 9, active high).
 *    DSM powers a bay from ata_port_probe() only when its presence pin
 *    reports a disk, in spin-up groups <2 1 1> spaced by
 *    syno_spinup_group_delay (11 s), then waits 3 s before probing the
 *    link. This driver is built in and does the same before ahci loads,
 *    then follows hot-plug like DSM's disk monitor (200 ms debounce, spin-up delay
 *    between insertions). At power-off the bays are switched off one by one
 *    500 ms apart, after the disks have been stopped, like
 *    DSM does. A reboot leaves them powered.
 *  - Bay presence: GPIO 10, 11, 13, 14, active low.
 *  - Fan tachometers: GPIO 68 and 69 (DSM fan_fail pins, pulse type).
 *    They are counted here and published as fan1/fan2 with an alarm when a
 *    fan stops.
 *  - I2C devices on AMDI0010 UID 3: Seiko S-35390A RTC (0x30, alarm on the
 *    INT1 pin that powers the NAS on, like DSM) and the ATmega1608 disk
 *    LED controller (0x60). The BIOS does not describe them in ACPI.
 *  - Front-panel microcontroller on UART AMDI0020:01 (MMIO 0xFEDCA000):
 *    DSM's reboot syscall writes 'C' (SYSTEM_RESET_FROM_CPU) before a
 *    restart and '1' (CPU_GRANT_PIC_TO_POWER_OFF_SYSTEM) before a power-off.
 *    Without them the board does not come back from a reset. The bytes are
 *    sent from the sys-off prepare hooks, after all devices are shut down,
 *    and 'C' also before an emergency restart (panic, sysrq-b).
 *  - A "synology" hwmon device with the two fans and pwm1, the duty the
 *    synohwd daemon sets on the microcontroller.
 */

#include <linux/acpi.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/hrtimer.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/hex.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reboot.h>
#include <linux/serial_reg.h>
#include <linux/workqueue.h>

#define DRV_NAME	"synology-nas"
#define MAX_BAYS	5
#define NUM_USB		2
#define NUM_FANS	2
#define UP_UART_BAUD	9600	/* the microcontroller only answers at 9600 */

/* r1000: microcontroller on AMDI0020:01, dw-apb-uart, reg-shift 2, 48 MHz clock */
#define AMD_UP_BASE	0xfedca000
#define AMD_UP_SIZE	0x100
#define AMD_UP_CLK	48000000

/*
 * geminilake: microcontroller on LPSS UART 0 (PCI 00:18.0), 16550 registers
 * with reg-shift 2 in BAR 0 and the LPSS private registers at 0x200. Its
 * 100 MHz clock goes through an M/N divider: 100 MHz * 576 / 31250 is
 * 1.8432 MHz, divisor 12 for 9600 baud.
 */
#define LPSS_UP_DEVFN	PCI_DEVFN(0x18, 0)
#define LPSS_PRIV	0x200
#define LPSS_CLOCK	(LPSS_PRIV + 0x00)	/* bit 0 enable, M 1-15, N 16-30, bit 31 update */
#define LPSS_RESETS	(LPSS_PRIV + 0x04)	/* bits 0-1: out of reset */
#define LPSS_UP_M	576
#define LPSS_UP_N	31250
#define LPSS_UP_CLK	1843200

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "Load as a DS923+ even if DMI does not report a supported model");

static char *spinup_groups;
module_param(spinup_groups, charp, 0444);
MODULE_PARM_DESC(spinup_groups, "Bays powered together per spin-up group (default: the model's, DS923+ 2,1,1)");

static unsigned int spinup_delay = 11;
module_param(spinup_delay, uint, 0444);
MODULE_PARM_DESC(spinup_delay, "Seconds between spin-up groups and hot-plugged disks (DSM: 11)");

static unsigned int settle_ms = 3000;
module_param(settle_ms, uint, 0444);
MODULE_PARM_DESC(settle_ms, "Wait after powering a group before SATA probe (DSM: 3000)");

static bool hotplug = true;
module_param(hotplug, bool, 0644);
MODULE_PARM_DESC(hotplug, "Power bays on insertion and off on removal");

static bool power_empty_bays;
module_param(power_empty_bays, bool, 0444);
MODULE_PARM_DESC(power_empty_bays, "Also power bays with no disk detected");

static unsigned int poll_ms = 200;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "Presence polling interval in ms, at least 50 (3 equal reads = debounce)");

static unsigned int syno_poll_ms(void)
{
	return max(READ_ONCE(poll_ms), 50U);
}

static bool usb_vbus = true;
module_param(usb_vbus, bool, 0444);
MODULE_PARM_DESC(usb_vbus, "Enable VBUS on the USB ports at boot");

static bool microp_shutdown = true;
module_param(microp_shutdown, bool, 0644);
MODULE_PARM_DESC(microp_shutdown, "Tell the microcontroller about restart ('C') and power-off ('1') like DSM");

static unsigned int fan_pulses = 2;
module_param(fan_pulses, uint, 0444);
MODULE_PARM_DESC(fan_pulses, "Tachometer pulses per fan revolution");

#define AMD_GPIO	"AMDI0030:00"
#define GLK_GPIO	"INT3453:00"	/* Gemini Lake community northwest (UID 1) */

static struct gpiod_lookup_table ds923plus_gpios = {
	.dev_id = DRV_NAME,
	.table = {
		GPIO_LOOKUP_IDX(AMD_GPIO, 42, "usb-vbus", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(AMD_GPIO, 40, "usb-vbus", 1, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(AMD_GPIO, 4, "power", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(AMD_GPIO, 5, "power", 1, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(AMD_GPIO, 8, "power", 2, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(AMD_GPIO, 9, "power", 3, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(AMD_GPIO, 10, "present", 0, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(AMD_GPIO, 11, "present", 1, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(AMD_GPIO, 13, "present", 2, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(AMD_GPIO, 14, "present", 3, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(AMD_GPIO, 68, "fan-tach", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(AMD_GPIO, 69, "fan-tach", 1, GPIO_ACTIVE_HIGH),
		{ }
	},
};

/* DS920+ (model.dtb of DSM 7.4.1, pins checked under DSM with a disk in bay 1) */
static struct gpiod_lookup_table ds920_gpios = {
	.dev_id = DRV_NAME,
	.table = {
		GPIO_LOOKUP_IDX(GLK_GPIO, 29, "usb-vbus", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GLK_GPIO, 30, "usb-vbus", 1, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GLK_GPIO, 20, "power", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GLK_GPIO, 21, "power", 1, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GLK_GPIO, 22, "power", 2, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GLK_GPIO, 23, "power", 3, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GLK_GPIO, 35, "present", 0, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(GLK_GPIO, 36, "present", 1, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(GLK_GPIO, 37, "present", 2, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(GLK_GPIO, 38, "present", 3, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(GLK_GPIO, 39, "fan-tach", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GLK_GPIO, 40, "fan-tach", 1, GPIO_ACTIVE_HIGH),
		/* DSM's disk_led_ctrl: the disk LEDs (LP3943) stay dark while it is low */
		GPIO_LOOKUP_IDX(GLK_GPIO, 17, "disk-led-ctrl", 0, GPIO_ACTIVE_HIGH),
		{ }
	},
};

/*
 * DS423+ (model.dtb of DSM 7.4.1): the same pins as the DS920+ (power_pin_gpio 20-23,
 * detect_pin_gpio 35-38, usb vbus 29/30); fan tachometers 39/40 and the disk LED enable 17
 * are configured the same way under DSM (inputs 39/40, output 17 high).
 */
#define ds423_gpios ds920_gpios

/* What differs between the models this driver supports. */
enum syno_up { UP_AMD, UP_LPSS };

struct syno_model {
	const char *name;
	unsigned int bays;		/* at most MAX_BAYS */
	const char *spinup_groups;	/* DSM's spin-up groups */
	struct gpiod_lookup_table *gpios;
	const char *gpio_chip;		/* for messages */
	/*
	 * Where the S-35390A RTC is: an I2C adapter (by the ACPI HID/UID of its
	 * controller) on which the client is created, or the HID of an ACPI
	 * description of it, whose client is replaced by an "s35390a" one.
	 */
	const char *rtc_bus_hid, *rtc_bus_uid;
	const char *rtc_acpi_hid;
	bool atmega_leds;		/* ATmega1608 disk LED controller at 0x60 */
	enum syno_up up;		/* microcontroller UART */
	/*
	 * Onboard NICs without a MAC address of their own, in the order of
	 * the macs= list (DSM's eth0, eth1): PCI device names.
	 */
	const char * const *nics;
};

static const struct syno_model ds923plus = {
	.name = "DS923+",
	.bays = 4,
	.spinup_groups = "2,1,1",
	.gpios = &ds923plus_gpios,
	.gpio_chip = AMD_GPIO,
	.rtc_bus_hid = "AMDI0010", .rtc_bus_uid = "3",
	.atmega_leds = true,
	.up = UP_AMD,
};

/* DSM: eth0 (LAN 1) is 03:00.0, eth1 (LAN 2) is 02:00.0 */
static const char * const ds920_nics[] = { "0000:03:00.0", "0000:02:00.0", NULL };

static const struct syno_model ds920plus = {
	.name = "DS920+",
	.bays = 4,
	.spinup_groups = "2,1,1",
	.gpios = &ds920_gpios,
	.gpio_chip = GLK_GPIO,
	.rtc_acpi_hid = "RTC35390",
	.up = UP_LPSS,
	.nics = ds920_nics,
};

/* DSM: eth0 (LAN 1) is 02:00.0, eth1 (LAN 2) is 03:00.0, the reverse of the DS920+ */
static const char * const ds423_nics[] = { "0000:02:00.0", "0000:03:00.0", NULL };

static const struct syno_model ds423plus = {
	.name = "DS423+",
	.bays = 4,
	.spinup_groups = "2,1,1",
	.gpios = &ds423_gpios,
	.gpio_chip = GLK_GPIO,
	.rtc_acpi_hid = "RTC35390",
	.up = UP_LPSS,
	.nics = ds423_nics,
};

/* the model found at init, used by probe and the I2C work */
static const struct syno_model *model;

struct syno_bay {
	struct gpio_desc *power;
	struct gpio_desc *present;
	bool state;		/* debounced presence */
	bool pending;		/* last raw read differs from state */
	unsigned int stable;	/* equal raw reads since the change */
};

struct syno {
	struct device *dev;
	unsigned int nbays;
	struct syno_bay bay[MAX_BAYS];
	struct gpio_desc *vbus[NUM_USB];
	struct gpio_desc *tach[NUM_FANS];
	struct gpio_desc *disk_led_ctrl;	/* optional */
	atomic_t pulses[NUM_FANS];
	long fan_rpm[NUM_FANS];
	/*
	 * Tachometers without an IRQ (DS423+: BIOS M.901.00 leaves GPIO 39/40 in ACPI
	 * mode, where pinctrl-intel refuses IRQs): sampled by tach_timer in windows.
	 */
	bool tach_polled[NUM_FANS];
	int tach_last[NUM_FANS];
	unsigned int tach_edges[NUM_FANS];
	unsigned int tach_left;
	bool tach_busy;
	struct hrtimer tach_timer;
	unsigned long fan_stamp;
	long fan_pwm;
	unsigned long last_power_on;
	struct mutex lock;		/* bay power and debounce state */
	struct delayed_work poll;
	void __iomem *up;
	struct pci_dev *up_pci;		/* LPSS UART kept powered (UP_LPSS) */
	struct notifier_block emergency_nb;
};

/* ---------------------------------------------------------------- bays */

static bool bay_powered(struct syno *ds, int bay)
{
	return ds->bay[bay].power && gpiod_get_value_cansleep(ds->bay[bay].power) > 0;
}

static bool bay_present(struct syno *ds, int bay)
{
	return ds->bay[bay].present && gpiod_get_value_cansleep(ds->bay[bay].present) > 0;
}

static int bay_set_power(struct syno *ds, int bay, bool on)
{
	int ret;

	if (!ds->bay[bay].power)
		return -ENODEV;
	ret = gpiod_set_value_cansleep(ds->bay[bay].power, on);
	if (ret)
		return ret;
	if (on)
		ds->last_power_on = jiffies;
	dev_info(ds->dev, "bay %d power %s\n", bay + 1, on ? "on" : "off");
	return 0;
}

/* Same order and timing as DSM's ata_port_probe() spin-up groups. */
static void syno_spinup(struct syno *ds)
{
	unsigned int nbays = ds->nbays, group[MAX_BAYS] = { 0 }, sum = 0;
	int ngroups = 0, bay = 0, g, i;
	char *s, *buf, *tok;

	buf = kstrdup(spinup_groups ?: model->spinup_groups, GFP_KERNEL);
	s = buf;
	while (buf && (tok = strsep(&s, ",")) && ngroups < nbays) {
		unsigned int n;

		if (kstrtouint(tok, 10, &n) || !n)
			continue;
		n = min_t(unsigned int, n, nbays);
		group[ngroups++] = n;
		sum += n;
	}
	kfree(buf);
	/* bays beyond the configured groups: one more group, delayed like the others */
	if (sum < nbays)
		group[ngroups++] = nbays - sum;

	for (g = 0; g < ngroups && bay < nbays; g++) {
		bool powered_any = false;

		for (i = 0; i < group[g] && bay < nbays; i++, bay++) {
			bool present = bay_present(ds, bay);

			ds->bay[bay].state = present;
			if (!ds->bay[bay].power)
				continue;
			if (bay_powered(ds, bay)) {
				dev_info(ds->dev, "bay %d already powered (%s)\n",
					 bay + 1, present ? "disk present" : "empty");
				continue;
			}
			if (!present && !power_empty_bays)
				continue;
			if (!powered_any && g > 0 && spinup_delay &&
			    time_before(jiffies, ds->last_power_on + spinup_delay * HZ)) {
				dev_info(ds->dev, "delay %u s before spin-up group %d\n",
					 spinup_delay, g + 1);
				ssleep(spinup_delay);
			}
			if (!bay_set_power(ds, bay, true))
				powered_any = true;
		}
		if (powered_any && settle_ms)
			msleep(settle_ms);
	}
}

static void syno_bays_poll(struct syno *ds)
{
	int bay;

	mutex_lock(&ds->lock);
	for (bay = 0; bay < ds->nbays; bay++) {
		struct syno_bay *b = &ds->bay[bay];
		bool present;

		if (!b->present)
			continue;
		present = bay_present(ds, bay);
		if (present == b->state) {
			b->pending = false;
			b->stable = 0;
			continue;
		}
		/* as DSM's disk monitor: act on a change only once it is stable */
		if (!b->pending) {
			b->pending = true;
			b->stable = 1;
			continue;
		}
		if (++b->stable < 3 || b->stable * syno_poll_ms() < 200)
			continue;
		if (present && hotplug && !power_empty_bays && b->power &&
		    !bay_powered(ds, bay) && spinup_delay &&
		    time_before(jiffies, ds->last_power_on + spinup_delay * HZ))
			continue;	/* another disk is spinning up: retry later */
		b->state = present;
		b->pending = false;
		b->stable = 0;
		dev_info(ds->dev, "bay %d: disk %s\n", bay + 1,
			 present ? "inserted" : "removed");
		if (!hotplug || !b->power)
			continue;
		if (present && !bay_powered(ds, bay))
			bay_set_power(ds, bay, true);
		else if (!present && !power_empty_bays && bay_powered(ds, bay))
			bay_set_power(ds, bay, false);
	}
	mutex_unlock(&ds->lock);
}

/* ---------------------------------------------------------------- fans */

static irqreturn_t syno_tach_irq(int irq, void *data)
{
	atomic_inc(data);
	return IRQ_HANDLED;
}

/*
 * Polled tachometers: 1 kHz for a 1 s window (a fan at 3000 rpm, 2 pulses per
 * revolution, still gives 5 samples per level), one window per fan cycle.
 */
#define TACH_SAMPLE_NS	NSEC_PER_MSEC
#define TACH_SAMPLES	1000

static enum hrtimer_restart syno_tach_sample(struct hrtimer *t)
{
	struct syno *ds = container_of(t, struct syno, tach_timer);
	int i;

	for (i = 0; i < NUM_FANS; i++) {
		int v;

		if (!ds->tach_polled[i])
			continue;
		v = gpiod_get_value(ds->tach[i]);
		if (v == 0 && ds->tach_last[i] == 1)
			ds->tach_edges[i]++;
		ds->tach_last[i] = v;
	}
	if (--ds->tach_left) {
		hrtimer_forward_now(t, ns_to_ktime(TACH_SAMPLE_NS));
		return HRTIMER_RESTART;
	}
	for (i = 0; i < NUM_FANS; i++)
		if (ds->tach_polled[i])
			WRITE_ONCE(ds->fan_rpm[i],
				   (long)ds->tach_edges[i] * 60 / max(fan_pulses, 1U));
	WRITE_ONCE(ds->tach_busy, false);
	return HRTIMER_NORESTART;
}

static void syno_fans_poll(struct syno *ds)
{
	unsigned long now = jiffies, elapsed = now - ds->fan_stamp;
	bool polled = false;
	int i;

	if (elapsed < 2 * HZ)
		return;
	for (i = 0; i < NUM_FANS; i++) {
		long n = atomic_xchg(&ds->pulses[i], 0);

		polled |= ds->tach_polled[i];
		if (ds->tach[i] && !ds->tach_polled[i])
			WRITE_ONCE(ds->fan_rpm[i],
				   n * 60 * HZ / (long)(elapsed * max(fan_pulses, 1U)));
	}
	ds->fan_stamp = now;
	if (polled && !READ_ONCE(ds->tach_busy)) {
		for (i = 0; i < NUM_FANS; i++) {
			ds->tach_edges[i] = 0;
			ds->tach_last[i] = -1;
		}
		ds->tach_left = TACH_SAMPLES;
		WRITE_ONCE(ds->tach_busy, true);
		hrtimer_start(&ds->tach_timer, ns_to_ktime(TACH_SAMPLE_NS), HRTIMER_MODE_REL);
	}
}

static void syno_poll(struct work_struct *work)
{
	struct syno *ds = container_of(to_delayed_work(work), struct syno, poll);

	syno_bays_poll(ds);
	syno_fans_poll(ds);
	queue_delayed_work(system_freezable_wq, &ds->poll, msecs_to_jiffies(syno_poll_ms()));
}

/* ---------------------------------------------------------------- microcontroller */

/* Bounded busy wait for an LSR bit, in 10 us steps. */
static void syno_up_wait(void __iomem *p, u32 bit, unsigned int us)
{
	unsigned int i;

	for (i = 0; i < us / 10 && !(readl(p + (UART_LSR << 2)) & bit); i++)
		udelay(10);
}

/*
 * Polled write on the microcontroller UART, used only once user space is
 * gone (sys-off prepare hooks). Divisor and line settings are reprogrammed
 * so the byte goes out at 9600 8N1 whatever the tty layer left behind.
 */
static void syno_up_write(struct syno *ds, u8 c)
{
	unsigned int clk = model->up == UP_LPSS ? LPSS_UP_CLK : AMD_UP_CLK;
	unsigned int div = DIV_ROUND_CLOSEST(clk, 16 * UP_UART_BAUD);
	void __iomem *p = ds->up;

	if (!p)
		return;
	/*
	 * Let whatever the tty layer queued go out first: changing the divisor
	 * mid-character garbles it. A full 64-byte FIFO at 9600 is ~67 ms.
	 */
	syno_up_wait(p, UART_LSR_TEMT, 100000);
	if (model->up == UP_LPSS) {
		u32 v = BIT(0) | (LPSS_UP_M << 1) | (LPSS_UP_N << 16);

		writel(3, p + LPSS_RESETS);
		writel(v, p + LPSS_CLOCK);
		writel(v | BIT(31), p + LPSS_CLOCK);
	}
	writel(UART_LCR_DLAB | UART_LCR_WLEN8, p + (UART_LCR << 2));
	writel(div & 0xff, p + (UART_DLL << 2));
	writel(div >> 8, p + (UART_DLM << 2));
	writel(UART_LCR_WLEN8, p + (UART_LCR << 2));
	syno_up_wait(p, UART_LSR_THRE, 10000);
	writel(c, p + (UART_TX << 2));
	syno_up_wait(p, UART_LSR_TEMT, 10000);
	mdelay(20);
}

static int syno_restart_prepare(struct sys_off_data *data)
{
	struct syno *ds = data->cb_data;

	if (microp_shutdown) {
		dev_info(ds->dev, "restart: notifying microcontroller\n");
		syno_up_write(ds, 'C');
	}
	return NOTIFY_DONE;
}

static int syno_emergency_restart(struct notifier_block *nb, unsigned long action, void *data)
{
	struct syno *ds = container_of(nb, struct syno, emergency_nb);

	if (microp_shutdown)
		syno_up_write(ds, 'C');	/* atomic context: no logging */
	return NOTIFY_DONE;
}

static void syno_emergency_unregister(void *data)
{
	unregister_emergency_restart_prep_notifier(data);
}

static int syno_power_off_prepare(struct sys_off_data *data)
{
	struct syno *ds = data->cb_data;

	if (microp_shutdown) {
		dev_info(ds->dev, "power-off: notifying microcontroller\n");
		syno_up_write(ds, '1');
	}
	return NOTIFY_DONE;
}

/* ---------------------------------------------------------------- sysfs */

static int attr_bay(struct device_attribute *attr)
{
	return attr->attr.name[3] - '1';
}

static ssize_t bay_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct syno *ds = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", bay_powered(ds, attr_bay(attr)));
}

static ssize_t bay_power_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct syno *ds = dev_get_drvdata(dev);
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	mutex_lock(&ds->lock);
	ret = bay_set_power(ds, attr_bay(attr), on);
	mutex_unlock(&ds->lock);
	return ret ?: count;
}

static ssize_t bay_present_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct syno *ds = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", bay_present(ds, attr_bay(attr)));
}

#define BAY_ATTRS(n)								\
	static struct device_attribute dev_attr_bay##n##_power =		\
		__ATTR(bay##n##_power, 0644, bay_power_show, bay_power_store);	\
	static struct device_attribute dev_attr_bay##n##_present =		\
		__ATTR(bay##n##_present, 0444, bay_present_show, NULL)
BAY_ATTRS(1);
BAY_ATTRS(2);
BAY_ATTRS(3);
BAY_ATTRS(4);
BAY_ATTRS(5);

static int vbus_get(struct syno *ds, int port)
{
	return ds->vbus[port] ? gpiod_get_value_cansleep(ds->vbus[port]) : -ENODEV;
}

static int vbus_set(struct syno *ds, int port, bool on)
{
	if (!ds->vbus[port])
		return -ENODEV;
	return gpiod_direction_output(ds->vbus[port], on);
}

/* usb_vbus: both slots ("a b" on read, one value for both on write) */
static ssize_t usb_vbus_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct syno *ds = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d %d\n", vbus_get(ds, 0), vbus_get(ds, 1));
}

static ssize_t usb_vbus_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct syno *ds = dev_get_drvdata(dev);
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	ret = vbus_set(ds, 0, on);
	if (!ret)
		ret = vbus_set(ds, 1, on);
	return ret ?: count;
}
static DEVICE_ATTR_RW(usb_vbus);

/* usbN_vbus: one slot (1 = GPIO 42, ports 1-2/2-2; 2 = GPIO 40, ports 1-3/2-3) */
static int attr_usb(struct device_attribute *attr)
{
	return attr->attr.name[3] - '1';
}

static ssize_t usb_slot_vbus_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n", vbus_get(dev_get_drvdata(dev), attr_usb(attr)));
}

static ssize_t usb_slot_vbus_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	ret = vbus_set(dev_get_drvdata(dev), attr_usb(attr), on);
	return ret ?: count;
}

static struct device_attribute dev_attr_usb1_vbus =
	__ATTR(usb1_vbus, 0644, usb_slot_vbus_show, usb_slot_vbus_store);
static struct device_attribute dev_attr_usb2_vbus =
	__ATTR(usb2_vbus, 0644, usb_slot_vbus_show, usb_slot_vbus_store);

static struct attribute *syno_attrs[] = {
	&dev_attr_bay1_power.attr, &dev_attr_bay1_present.attr,
	&dev_attr_bay2_power.attr, &dev_attr_bay2_present.attr,
	&dev_attr_bay3_power.attr, &dev_attr_bay3_present.attr,
	&dev_attr_bay4_power.attr, &dev_attr_bay4_present.attr,
	&dev_attr_bay5_power.attr, &dev_attr_bay5_present.attr,
	&dev_attr_usb_vbus.attr, &dev_attr_usb1_vbus.attr, &dev_attr_usb2_vbus.attr,
	NULL
};

/* bayN_* only for the bays the model has */
static umode_t syno_attr_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct syno *ds = dev_get_drvdata(kobj_to_dev(kobj));

	if (!strncmp(attr->name, "bay", 3) && attr->name[3] - '1' >= ds->nbays)
		return 0;
	return attr->mode;
}

static const struct attribute_group syno_group = {
	.attrs = syno_attrs,
	.is_visible = syno_attr_visible,
};
__ATTRIBUTE_GROUPS(syno);

/* ---------------------------------------------------------------- hwmon */

/* fan1/fan2: tachometers; pwm1: duty set by synohwd on the microcontroller */
static umode_t syno_hwmon_visible(const void *data, enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	const struct syno *ds = data;

	if (type == hwmon_fan)
		return ds->tach[channel] ? 0444 : 0;
	if (type == hwmon_pwm && attr == hwmon_pwm_input)
		return 0644;
	return 0;
}

static int syno_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *val)
{
	struct syno *ds = dev_get_drvdata(dev);

	if (type == hwmon_fan) {
		long rpm = READ_ONCE(ds->fan_rpm[channel]);

		if (rpm < 0)
			return -ENODATA;
		*val = attr == hwmon_fan_alarm ? rpm == 0 : rpm;
		return 0;
	}
	*val = READ_ONCE(ds->fan_pwm);
	return 0;
}

static int syno_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long val)
{
	struct syno *ds = dev_get_drvdata(dev);

	WRITE_ONCE(ds->fan_pwm, clamp_val(val, 0, 255));
	return 0;
}

static const struct hwmon_ops syno_hwmon_ops = {
	.is_visible = syno_hwmon_visible,
	.read = syno_hwmon_read,
	.write = syno_hwmon_write,
};

static const struct hwmon_channel_info * const syno_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_ALARM, HWMON_F_INPUT | HWMON_F_ALARM),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_chip_info syno_hwmon_chip = {
	.ops = &syno_hwmon_ops,
	.info = syno_hwmon_info,
};

/* ---------------------------------------------------------------- I2C */

/* The S-35390A INT1 output is the one wired to the power-on logic. */
static const struct property_entry syno_rtc_props[] = {
	PROPERTY_ENTRY_BOOL("sii,alarm-int1"),
	{ }
};

static const struct software_node syno_rtc_swnode = {
	.properties = syno_rtc_props,
};

/*
 * r1000: the I2C bus AMDI0010 with _UID 3 carries the RTC and the LED
 * controller, which the BIOS does not describe.
 */
static bool is_syno_adapter(struct device *dev)
{
	struct i2c_adapter *adap = i2c_verify_adapter(dev);
	struct acpi_device *adev;

	if (!model->rtc_bus_hid || !adap || !adap->dev.parent)
		return false;
	adev = ACPI_COMPANION(adap->dev.parent);
	return adev && acpi_dev_hid_uid_match(adev, model->rtc_bus_hid, model->rtc_bus_uid);
}

/*
 * geminilake: the BIOS describes the RTC in ACPI (RTC35390), but no RTC
 * driver knows that ID, and the S-35390A INT1 pin needs a property. The
 * ACPI client is an empty shell: it is replaced by an "s35390a" client at
 * the same address, with the property.
 */
static bool is_rtc_acpi_client(struct device *dev)
{
	struct i2c_client *client = i2c_verify_client(dev);
	struct acpi_device *adev;

	if (!model->rtc_acpi_hid || !client || client->dev.driver)
		return false;
	adev = ACPI_COMPANION(&client->dev);
	return adev && acpi_dev_hid_uid_match(adev, model->rtc_acpi_hid, NULL);
}

static int match_rtc_acpi_client(struct device *dev, const void *data)
{
	return is_rtc_acpi_client(dev);
}

static int match_adapter(struct device *dev, const void *data)
{
	return is_syno_adapter(dev);
}

static int match_client_addr(struct device *dev, void *data)
{
	struct i2c_client *client = i2c_verify_client(dev);

	return client && client->addr == *(unsigned short *)data;
}

/* Create @info on @adap unless the address already has a client. */
static void syno_i2c_add(struct i2c_adapter *adap, struct i2c_board_info *info,
			  const char *what)
{
	struct i2c_client *client;

	if (device_for_each_child(&adap->dev, &info->addr, match_client_addr))
		return;
	client = i2c_new_client_device(adap, info);
	if (IS_ERR(client))
		pr_warn(DRV_NAME ": %s: %pe\n", what, client);
}

/*
 * The clients belong to the adapter, not to this driver: no pointer to them
 * is kept, they go away with the adapter and are created again when it comes
 * back. Only ever run from syno_i2c_work, so never concurrently.
 */
static void syno_i2c_register(struct work_struct *work)
{
	struct i2c_board_info rtc = {
		I2C_BOARD_INFO("s35390a", 0x30),
		.swnode = &syno_rtc_swnode,
	};
	struct i2c_board_info leds = { I2C_BOARD_INFO("atmega1608", 0x60) };
	struct device *dev;

	dev = bus_find_device(&i2c_bus_type, NULL, NULL, match_rtc_acpi_client);
	if (dev) {
		struct i2c_client *client = i2c_verify_client(dev);
		struct i2c_adapter *adap = i2c_get_adapter(i2c_adapter_id(client->adapter));

		rtc.addr = client->addr;
		if (adap)
			i2c_unregister_device(client);
		put_device(dev);
		if (adap) {
			syno_i2c_add(adap, &rtc, "S-35390A RTC");
			i2c_put_adapter(adap);
		}
	}

	dev = bus_find_device(&i2c_bus_type, NULL, NULL, match_adapter);
	if (!dev)
		return;
	syno_i2c_add(i2c_verify_adapter(dev), &rtc, "S-35390A RTC");
	if (model->atmega_leds)
		syno_i2c_add(i2c_verify_adapter(dev), &leds, "ATmega1608 LEDs");
	put_device(dev);
}

static DECLARE_WORK(syno_i2c_work, syno_i2c_register);

/*
 * The DesignWare I2C controller normally probes at subsys_initcall, before
 * this driver; if it shows up later, or again after an unbind, add the
 * clients from the work item (not from the notifier itself, which runs inside
 * the adapter's device_add()).
 */
static int syno_i2c_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	if (action == BUS_NOTIFY_ADD_DEVICE &&
	    (is_syno_adapter(data) || is_rtc_acpi_client(data)))
		schedule_work(&syno_i2c_work);
	return NOTIFY_DONE;
}

static struct notifier_block syno_i2c_nb = {
	.notifier_call = syno_i2c_notify,
};

/* ---------------------------------------------------------------- MAC addresses */

#define MAX_NICS	4
static u8 syno_macs[MAX_NICS][ETH_ALEN];
static unsigned int syno_nmacs;

/* macs=001132aabbcc,001132aabbcd: added by the Synology GRUB ("vender" command) */
static int __init syno_macs_setup(char *str)
{
	while (str && *str && syno_nmacs < MAX_NICS) {
		char *next = strchr(str, ',');

		if (next)
			*next++ = '\0';
		if (strlen(str) == 2 * ETH_ALEN &&
		    !hex2bin(syno_macs[syno_nmacs], str, ETH_ALEN) &&
		    is_valid_ether_addr(syno_macs[syno_nmacs]))
			syno_nmacs++;
		str = next;
	}
	return 1;
}
__setup("macs=", syno_macs_setup);

static int syno_netdev_event(struct notifier_block *nb, unsigned long action, void *ptr)
{
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);
	struct device *parent = ndev->dev.parent;
	struct sockaddr_storage ss = { .ss_family = ndev->type };
	unsigned int i;
	int ret;

	if (action != NETDEV_REGISTER || !parent || !dev_is_pci(parent))
		return NOTIFY_DONE;
	for (i = 0; model->nics[i] && i < syno_nmacs; i++) {
		if (strcmp(dev_name(parent), model->nics[i]))
			continue;
		memcpy(ss.__data, syno_macs[i], ETH_ALEN);
		ret = dev_set_mac_address(ndev, &ss, NULL);
		if (ret)
			pr_warn(DRV_NAME ": %s: MAC address from macs=: %d\n", ndev->name, ret);
		else
			pr_info(DRV_NAME ": %s (%s): MAC address %pM from macs=\n",
				ndev->name, model->nics[i], syno_macs[i]);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block syno_netdev_nb = {
	.notifier_call = syno_netdev_event,
};

/* ---------------------------------------------------------------- probe */

static void syno_up_release(void *data)
{
	struct pci_dev *pdev = data;

	pm_runtime_put(&pdev->dev);
	pci_dev_put(pdev);
}

/*
 * Map the microcontroller UART for the polled writes at restart/power-off.
 * The LPSS UART is a PCI device that runtime PM may power down when its tty
 * is closed (as it is at power-off): it is kept powered from here on.
 */
static void syno_up_map(struct syno *ds)
{
	struct pci_dev *up;

	if (model->up == UP_AMD) {
		ds->up = devm_ioremap(ds->dev, AMD_UP_BASE, AMD_UP_SIZE);
	} else {
		up = pci_get_domain_bus_and_slot(0, 0, LPSS_UP_DEVFN);
		if (up && up->vendor == PCI_VENDOR_ID_INTEL &&
		    pci_resource_len(up, 0) >= LPSS_PRIV + 0x10) {
			pm_runtime_get_sync(&up->dev);
			if (devm_add_action_or_reset(ds->dev, syno_up_release, up))
				return;
			ds->up_pci = up;
			ds->up = devm_ioremap(ds->dev, pci_resource_start(up, 0),
					      LPSS_PRIV + 0x10);
		} else {
			pci_dev_put(up);
		}
	}
	if (!ds->up)
		dev_warn(ds->dev, "microcontroller UART not mapped\n");
}

static int syno_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *hwmon;
	struct syno *ds;
	int i, ret;

	ds = devm_kzalloc(dev, sizeof(*ds), GFP_KERNEL);
	if (!ds)
		return -ENOMEM;
	ds->dev = dev;
	ds->nbays = model->bays;
	mutex_init(&ds->lock);
	ds->last_power_on = jiffies - spinup_delay * HZ - 1;
	platform_set_drvdata(pdev, ds);

	/* USB first: the boot device may sit behind these ports. */
	for (i = 0; i < NUM_USB; i++) {
		ds->vbus[i] = devm_gpiod_get_index(dev, "usb-vbus", i, GPIOD_ASIS);
		/* pinctrl-amd not bound yet: come back later, never give up on USB */
		if (PTR_ERR(ds->vbus[i]) == -EPROBE_DEFER)
			return dev_err_probe(dev, -EPROBE_DEFER, "waiting for %s\n",
					     model->gpio_chip);
		if (IS_ERR(ds->vbus[i])) {
			dev_err(dev, "usb vbus gpio %d: %pe\n", i, ds->vbus[i]);
			ds->vbus[i] = NULL;
			continue;
		}
		if (usb_vbus) {
			ret = gpiod_direction_output(ds->vbus[i], 1);
			if (ret)
				dev_err(dev, "usb vbus %d on: %d\n", i, ret);
		}
	}

	for (i = 0; i < ds->nbays; i++) {
		struct syno_bay *b = &ds->bay[i];

		b->present = devm_gpiod_get_index(dev, "present", i, GPIOD_IN);
		if (IS_ERR(b->present)) {
			dev_err(dev, "bay %d presence gpio: %pe\n", i + 1, b->present);
			b->present = NULL;
		}
		b->power = devm_gpiod_get_index(dev, "power", i, GPIOD_ASIS);
		if (IS_ERR(b->power)) {
			dev_err(dev, "bay %d power gpio: %pe\n", i + 1, b->power);
			b->power = NULL;
			continue;
		}
		/* Keep the current level: a warm reboot leaves bays on. */
		ret = gpiod_direction_output(b->power, gpiod_get_value_cansleep(b->power) > 0);
		if (ret) {
			dev_err(dev, "bay %d power output: %d\n", i + 1, ret);
			b->power = NULL;
		}
	}

	/* models with a disk LED enable (DS920+): on, as DSM leaves it */
	ds->disk_led_ctrl = devm_gpiod_get_optional(dev, "disk-led-ctrl", GPIOD_OUT_HIGH);
	if (IS_ERR(ds->disk_led_ctrl)) {
		dev_warn(dev, "disk LED enable gpio: %pe\n", ds->disk_led_ctrl);
		ds->disk_led_ctrl = NULL;
	}

	for (i = 0; i < NUM_FANS; i++) {
		int irq;

		ds->fan_rpm[i] = -1;
		ds->tach[i] = devm_gpiod_get_index(dev, "fan-tach", i, GPIOD_IN);
		if (IS_ERR(ds->tach[i])) {
			dev_warn(dev, "fan %d tach gpio: %pe\n", i + 1, ds->tach[i]);
			ds->tach[i] = NULL;
			continue;
		}
		irq = gpiod_to_irq(ds->tach[i]);
		ret = irq < 0 ? irq : devm_request_irq(dev, irq, syno_tach_irq,
						       IRQF_TRIGGER_FALLING,
						       i ? "syno-fan2" : "syno-fan1",
						       &ds->pulses[i]);
		if (ret) {
			dev_info(dev, "fan %d tach irq: %d, polling the gpio\n", i + 1, ret);
			ds->tach_polled[i] = true;
		}
	}
	hrtimer_setup(&ds->tach_timer, syno_tach_sample, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ds->fan_stamp = jiffies;

	syno_up_map(ds);
	/*
	 * PRIO_DEFAULT: for the *_PREPARE chains any other priority must be unique, and
	 * PRIO_PLATFORM is already taken on the DS923+ (-EBUSY).
	 */
	ret = devm_register_sys_off_handler(dev, SYS_OFF_MODE_RESTART_PREPARE,
					    SYS_OFF_PRIO_DEFAULT, syno_restart_prepare, ds);
	if (!ret)
		ret = devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF_PREPARE,
						    SYS_OFF_PRIO_DEFAULT,
						    syno_power_off_prepare, ds);
	if (ret)
		dev_warn(dev, "sys-off handlers: %d\n", ret);
	ds->emergency_nb.notifier_call = syno_emergency_restart;
	if (!register_emergency_restart_prep_notifier(&ds->emergency_nb))
		devm_add_action_or_reset(dev, syno_emergency_unregister, &ds->emergency_nb);

	syno_spinup(ds);

	hwmon = devm_hwmon_device_register_with_info(dev, "synology", ds, &syno_hwmon_chip, NULL);
	if (IS_ERR(hwmon))
		dev_warn(dev, "hwmon: %pe\n", hwmon);

	INIT_DELAYED_WORK(&ds->poll, syno_poll);
	queue_delayed_work(system_freezable_wq, &ds->poll, msecs_to_jiffies(syno_poll_ms()));
	dev_info(dev, "Synology %s platform ready\n", model->name);
	return 0;
}

static void syno_remove(struct platform_device *pdev)
{
	struct syno *ds = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&ds->poll);
	hrtimer_cancel(&ds->tach_timer);
	/* Bay and USB power are left as they are: unbinding must not cut the disks. */
}

static void syno_shutdown(struct platform_device *pdev)
{
	struct syno *ds = platform_get_drvdata(pdev);
	int bay;

	cancel_delayed_work_sync(&ds->poll);
	hrtimer_cancel(&ds->tach_timer);
	if (system_state != SYSTEM_POWER_OFF)
		return;
	/* The disks are already stopped by sd_shutdown(). */
	for (bay = 0; bay < ds->nbays; bay++) {
		if (!bay_powered(ds, bay))
			continue;
		bay_set_power(ds, bay, false);
		msleep(500);
	}
}

static struct platform_driver syno_driver = {
	.driver = {
		.name = DRV_NAME,
		.dev_groups = syno_groups,
		/* the spin-up sleeps: let other built-in drivers go on meanwhile */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = syno_probe,
	.remove = syno_remove,
	.shutdown = syno_shutdown,
};

static const struct dmi_system_id syno_dmi[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Synology Inc."),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "DS923+"),
		},
		.driver_data = (void *)&ds923plus,
	},
	{
		/* this BIOS (M.412.00) says "Synology", not "Synology Inc." */
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Synology"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "DS920+"),
		},
		.driver_data = (void *)&ds920plus,
	},
	{
		/* BIOS M.901.00: vendor "Synology", as on the DS920+ */
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Synology"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "DS423+"),
		},
		.driver_data = (void *)&ds423plus,
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, syno_dmi);

/* Built in only (Kconfig bool): there is no exit path. */
static int __init syno_init(void)
{
	const struct dmi_system_id *id = dmi_first_match(syno_dmi);
	struct platform_device *pdev;
	int ret;

	if (id)
		model = id->driver_data;
	else if (force)
		model = &ds923plus;
	else
		return -ENODEV;
	if (model->bays > MAX_BAYS)
		return -EINVAL;

	/*
	 * The notifier stays registered for good, so the clients come back
	 * whenever the adapter does. The first scan is waited for, so that
	 * the RTC exists (as rtc0: there is no other RTC driver on this board)
	 * before rtc_hctosys runs at late_initcall.
	 */
	ret = bus_register_notifier(&i2c_bus_type, &syno_i2c_nb);
	if (ret)
		pr_warn(DRV_NAME ": i2c bus notifier: %d\n", ret);
	schedule_work(&syno_i2c_work);
	flush_work(&syno_i2c_work);

	/* NIC drivers are modules: their devices register long after this */
	if (model->nics && syno_nmacs) {
		ret = register_netdevice_notifier(&syno_netdev_nb);
		if (ret)
			pr_warn(DRV_NAME ": netdevice notifier: %d\n", ret);
	}

	gpiod_add_lookup_table(model->gpios);
	ret = platform_driver_register(&syno_driver);
	if (ret)
		goto err_table;
	pdev = platform_device_register_simple(DRV_NAME, PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto err_drv;
	}
	return 0;

	/* the RTC and LED clients stay: they do not depend on the platform device */
err_drv:
	platform_driver_unregister(&syno_driver);
err_table:
	gpiod_remove_lookup_table(model->gpios);
	return ret;
}
module_init(syno_init);

MODULE_AUTHOR("Marco Ziliani <508111+rylos@users.noreply.github.com>");
MODULE_DESCRIPTION("Synology NAS platform (DS923+, DS920+, DS423+): bays, USB VBUS, fans, RTC/LED devices, microcontroller shutdown");
MODULE_LICENSE("GPL");
