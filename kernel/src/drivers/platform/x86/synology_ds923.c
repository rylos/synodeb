// SPDX-License-Identifier: GPL-2.0
/*
 * Synology DS923+ platform driver.
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
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reboot.h>
#include <linux/serial_reg.h>
#include <linux/workqueue.h>

#define DRV_NAME	"synology-ds923"
#define NUM_BAYS	4
#define NUM_USB		2
#define NUM_FANS	2
#define GPIO_CHIP	"AMDI0030:00"

/* Microcontroller UART: dw-apb-uart, reg-shift 2, 48 MHz clock */
#define UP_UART_BASE	0xfedca000
#define UP_UART_SIZE	0x100
#define UP_UART_CLK	48000000
#define UP_UART_BAUD	9600	/* the microcontroller only answers at 9600 */

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "Load even if DMI does not report a Synology DS923+");

static char *spinup_groups = "2,1,1";
module_param(spinup_groups, charp, 0444);
MODULE_PARM_DESC(spinup_groups, "Bays powered together per spin-up group (DSM: 2,1,1)");

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

static unsigned int ds923_poll_ms(void)
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

static struct gpiod_lookup_table ds923_gpios = {
	.dev_id = DRV_NAME,
	.table = {
		GPIO_LOOKUP_IDX(GPIO_CHIP, 42, "usb-vbus", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GPIO_CHIP, 40, "usb-vbus", 1, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GPIO_CHIP, 4, "power", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GPIO_CHIP, 5, "power", 1, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GPIO_CHIP, 8, "power", 2, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GPIO_CHIP, 9, "power", 3, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GPIO_CHIP, 10, "present", 0, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(GPIO_CHIP, 11, "present", 1, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(GPIO_CHIP, 13, "present", 2, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(GPIO_CHIP, 14, "present", 3, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX(GPIO_CHIP, 68, "fan-tach", 0, GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP_IDX(GPIO_CHIP, 69, "fan-tach", 1, GPIO_ACTIVE_HIGH),
		{ }
	},
};

struct ds923_bay {
	struct gpio_desc *power;
	struct gpio_desc *present;
	bool state;		/* debounced presence */
	bool pending;		/* last raw read differs from state */
	unsigned int stable;	/* equal raw reads since the change */
};

struct ds923 {
	struct device *dev;
	struct ds923_bay bay[NUM_BAYS];
	struct gpio_desc *vbus[NUM_USB];
	struct gpio_desc *tach[NUM_FANS];
	atomic_t pulses[NUM_FANS];
	long fan_rpm[NUM_FANS];
	unsigned long fan_stamp;
	long fan_pwm;
	unsigned long last_power_on;
	struct mutex lock;		/* bay power and debounce state */
	struct delayed_work poll;
	void __iomem *up;
	struct notifier_block emergency_nb;
};

/* ---------------------------------------------------------------- bays */

static bool bay_powered(struct ds923 *ds, int bay)
{
	return ds->bay[bay].power && gpiod_get_value_cansleep(ds->bay[bay].power) > 0;
}

static bool bay_present(struct ds923 *ds, int bay)
{
	return ds->bay[bay].present && gpiod_get_value_cansleep(ds->bay[bay].present) > 0;
}

static int bay_set_power(struct ds923 *ds, int bay, bool on)
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
static void ds923_spinup(struct ds923 *ds)
{
	unsigned int group[NUM_BAYS] = { 0 }, sum = 0;
	int ngroups = 0, bay = 0, g, i;
	char *s, *buf, *tok;

	buf = kstrdup(spinup_groups, GFP_KERNEL);
	s = buf;
	while (buf && (tok = strsep(&s, ",")) && ngroups < NUM_BAYS) {
		unsigned int n;

		if (kstrtouint(tok, 10, &n) || !n)
			continue;
		n = min_t(unsigned int, n, NUM_BAYS);
		group[ngroups++] = n;
		sum += n;
	}
	kfree(buf);
	/* bays beyond the configured groups: one more group, delayed like the others */
	if (sum < NUM_BAYS)
		group[ngroups++] = NUM_BAYS - sum;

	for (g = 0; g < ngroups && bay < NUM_BAYS; g++) {
		bool powered_any = false;

		for (i = 0; i < group[g] && bay < NUM_BAYS; i++, bay++) {
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

static void ds923_bays_poll(struct ds923 *ds)
{
	int bay;

	mutex_lock(&ds->lock);
	for (bay = 0; bay < NUM_BAYS; bay++) {
		struct ds923_bay *b = &ds->bay[bay];
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
		if (++b->stable < 3 || b->stable * ds923_poll_ms() < 200)
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

static irqreturn_t ds923_tach_irq(int irq, void *data)
{
	atomic_inc(data);
	return IRQ_HANDLED;
}

static void ds923_fans_poll(struct ds923 *ds)
{
	unsigned long now = jiffies, elapsed = now - ds->fan_stamp;
	int i;

	if (elapsed < 2 * HZ)
		return;
	for (i = 0; i < NUM_FANS; i++) {
		long n = atomic_xchg(&ds->pulses[i], 0);

		if (ds->tach[i])
			WRITE_ONCE(ds->fan_rpm[i],
				   n * 60 * HZ / (long)(elapsed * max(fan_pulses, 1U)));
	}
	ds->fan_stamp = now;
}

static void ds923_poll(struct work_struct *work)
{
	struct ds923 *ds = container_of(to_delayed_work(work), struct ds923, poll);

	ds923_bays_poll(ds);
	ds923_fans_poll(ds);
	queue_delayed_work(system_freezable_wq, &ds->poll, msecs_to_jiffies(ds923_poll_ms()));
}

/* ---------------------------------------------------------------- microcontroller */

/* Bounded busy wait for an LSR bit, in 10 us steps. */
static void ds923_up_wait(void __iomem *p, u32 bit, unsigned int us)
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
static void ds923_up_write(struct ds923 *ds, u8 c)
{
	unsigned int div = DIV_ROUND_CLOSEST(UP_UART_CLK, 16 * UP_UART_BAUD);
	void __iomem *p = ds->up;

	if (!p)
		return;
	/*
	 * Let whatever the tty layer queued go out first: changing the divisor
	 * mid-character garbles it. A full 64-byte FIFO at 9600 is ~67 ms.
	 */
	ds923_up_wait(p, UART_LSR_TEMT, 100000);
	writel(UART_LCR_DLAB | UART_LCR_WLEN8, p + (UART_LCR << 2));
	writel(div & 0xff, p + (UART_DLL << 2));
	writel(div >> 8, p + (UART_DLM << 2));
	writel(UART_LCR_WLEN8, p + (UART_LCR << 2));
	ds923_up_wait(p, UART_LSR_THRE, 10000);
	writel(c, p + (UART_TX << 2));
	ds923_up_wait(p, UART_LSR_TEMT, 10000);
	mdelay(20);
}

static int ds923_restart_prepare(struct sys_off_data *data)
{
	struct ds923 *ds = data->cb_data;

	if (microp_shutdown) {
		dev_info(ds->dev, "restart: notifying microcontroller\n");
		ds923_up_write(ds, 'C');
	}
	return NOTIFY_DONE;
}

static int ds923_emergency_restart(struct notifier_block *nb, unsigned long action, void *data)
{
	struct ds923 *ds = container_of(nb, struct ds923, emergency_nb);

	if (microp_shutdown)
		ds923_up_write(ds, 'C');	/* atomic context: no logging */
	return NOTIFY_DONE;
}

static void ds923_emergency_unregister(void *data)
{
	unregister_emergency_restart_prep_notifier(data);
}

static int ds923_power_off_prepare(struct sys_off_data *data)
{
	struct ds923 *ds = data->cb_data;

	if (microp_shutdown) {
		dev_info(ds->dev, "power-off: notifying microcontroller\n");
		ds923_up_write(ds, '1');
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
	struct ds923 *ds = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", bay_powered(ds, attr_bay(attr)));
}

static ssize_t bay_power_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct ds923 *ds = dev_get_drvdata(dev);
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
	struct ds923 *ds = dev_get_drvdata(dev);

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

static int vbus_get(struct ds923 *ds, int port)
{
	return ds->vbus[port] ? gpiod_get_value_cansleep(ds->vbus[port]) : -ENODEV;
}

static int vbus_set(struct ds923 *ds, int port, bool on)
{
	if (!ds->vbus[port])
		return -ENODEV;
	return gpiod_direction_output(ds->vbus[port], on);
}

/* usb_vbus: both slots ("a b" on read, one value for both on write) */
static ssize_t usb_vbus_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ds923 *ds = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d %d\n", vbus_get(ds, 0), vbus_get(ds, 1));
}

static ssize_t usb_vbus_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct ds923 *ds = dev_get_drvdata(dev);
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

static struct attribute *ds923_attrs[] = {
	&dev_attr_bay1_power.attr, &dev_attr_bay1_present.attr,
	&dev_attr_bay2_power.attr, &dev_attr_bay2_present.attr,
	&dev_attr_bay3_power.attr, &dev_attr_bay3_present.attr,
	&dev_attr_bay4_power.attr, &dev_attr_bay4_present.attr,
	&dev_attr_usb_vbus.attr, &dev_attr_usb1_vbus.attr, &dev_attr_usb2_vbus.attr,
	NULL
};
ATTRIBUTE_GROUPS(ds923);

/* ---------------------------------------------------------------- hwmon */

/* fan1/fan2: tachometers; pwm1: duty set by synohwd on the microcontroller */
static umode_t ds923_hwmon_visible(const void *data, enum hwmon_sensor_types type,
				   u32 attr, int channel)
{
	const struct ds923 *ds = data;

	if (type == hwmon_fan)
		return ds->tach[channel] ? 0444 : 0;
	if (type == hwmon_pwm && attr == hwmon_pwm_input)
		return 0644;
	return 0;
}

static int ds923_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *val)
{
	struct ds923 *ds = dev_get_drvdata(dev);

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

static int ds923_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long val)
{
	struct ds923 *ds = dev_get_drvdata(dev);

	WRITE_ONCE(ds->fan_pwm, clamp_val(val, 0, 255));
	return 0;
}

static const struct hwmon_ops ds923_hwmon_ops = {
	.is_visible = ds923_hwmon_visible,
	.read = ds923_hwmon_read,
	.write = ds923_hwmon_write,
};

static const struct hwmon_channel_info * const ds923_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_ALARM, HWMON_F_INPUT | HWMON_F_ALARM),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_chip_info ds923_hwmon_chip = {
	.ops = &ds923_hwmon_ops,
	.info = ds923_hwmon_info,
};

/* ---------------------------------------------------------------- I2C */

/* The S-35390A INT1 output is the one wired to the power-on logic. */
static const struct property_entry ds923_rtc_props[] = {
	PROPERTY_ENTRY_BOOL("sii,alarm-int1"),
	{ }
};

static const struct software_node ds923_rtc_swnode = {
	.properties = ds923_rtc_props,
};

/* I2C bus AMDI0010 with _UID 3 carries the RTC and the LED controller. */
static bool is_ds923_adapter(struct device *dev)
{
	struct i2c_adapter *adap = i2c_verify_adapter(dev);
	struct acpi_device *adev;

	if (!adap || !adap->dev.parent)
		return false;
	adev = ACPI_COMPANION(adap->dev.parent);
	return adev && acpi_dev_hid_uid_match(adev, "AMDI0010", "3");
}

static int match_adapter(struct device *dev, const void *data)
{
	return is_ds923_adapter(dev);
}

static int match_client_addr(struct device *dev, void *data)
{
	struct i2c_client *client = i2c_verify_client(dev);

	return client && client->addr == *(unsigned short *)data;
}

/* Create @info on @adap unless the address already has a client. */
static void ds923_i2c_add(struct i2c_adapter *adap, struct i2c_board_info *info,
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
 * back. Only ever run from ds923_i2c_work, so never concurrently.
 */
static void ds923_i2c_register(struct work_struct *work)
{
	struct i2c_board_info rtc = {
		I2C_BOARD_INFO("s35390a", 0x30),
		.swnode = &ds923_rtc_swnode,
	};
	struct i2c_board_info leds = { I2C_BOARD_INFO("atmega1608", 0x60) };
	struct device *dev;

	dev = bus_find_device(&i2c_bus_type, NULL, NULL, match_adapter);
	if (!dev)
		return;
	ds923_i2c_add(i2c_verify_adapter(dev), &rtc, "S-35390A RTC");
	ds923_i2c_add(i2c_verify_adapter(dev), &leds, "ATmega1608 LEDs");
	put_device(dev);
}

static DECLARE_WORK(ds923_i2c_work, ds923_i2c_register);

/*
 * The DesignWare I2C controller normally probes at subsys_initcall, before
 * this driver; if it shows up later, or again after an unbind, add the
 * clients from the work item (not from the notifier itself, which runs inside
 * the adapter's device_add()).
 */
static int ds923_i2c_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	if (action == BUS_NOTIFY_ADD_DEVICE && is_ds923_adapter(data))
		schedule_work(&ds923_i2c_work);
	return NOTIFY_DONE;
}

static struct notifier_block ds923_i2c_nb = {
	.notifier_call = ds923_i2c_notify,
};

/* ---------------------------------------------------------------- probe */

static int ds923_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *hwmon;
	struct ds923 *ds;
	int i, ret;

	ds = devm_kzalloc(dev, sizeof(*ds), GFP_KERNEL);
	if (!ds)
		return -ENOMEM;
	ds->dev = dev;
	mutex_init(&ds->lock);
	ds->last_power_on = jiffies - spinup_delay * HZ - 1;
	platform_set_drvdata(pdev, ds);

	/* USB first: the boot device may sit behind these ports. */
	for (i = 0; i < NUM_USB; i++) {
		ds->vbus[i] = devm_gpiod_get_index(dev, "usb-vbus", i, GPIOD_ASIS);
		/* pinctrl-amd not bound yet: come back later, never give up on USB */
		if (PTR_ERR(ds->vbus[i]) == -EPROBE_DEFER)
			return dev_err_probe(dev, -EPROBE_DEFER, "waiting for %s\n", GPIO_CHIP);
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

	for (i = 0; i < NUM_BAYS; i++) {
		struct ds923_bay *b = &ds->bay[i];

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
		ret = irq < 0 ? irq : devm_request_irq(dev, irq, ds923_tach_irq,
						       IRQF_TRIGGER_FALLING,
						       i ? "ds923-fan2" : "ds923-fan1",
						       &ds->pulses[i]);
		if (ret) {
			dev_warn(dev, "fan %d tach irq: %d\n", i + 1, ret);
			ds->tach[i] = NULL;
		}
	}
	ds->fan_stamp = jiffies;

	ds->up = devm_ioremap(dev, UP_UART_BASE, UP_UART_SIZE);
	if (!ds->up)
		dev_warn(dev, "microcontroller UART not mapped\n");
	/*
	 * PRIO_DEFAULT: for the *_PREPARE chains any other priority must be unique, and
	 * PRIO_PLATFORM is already taken on the DS923+ (-EBUSY).
	 */
	ret = devm_register_sys_off_handler(dev, SYS_OFF_MODE_RESTART_PREPARE,
					    SYS_OFF_PRIO_DEFAULT, ds923_restart_prepare, ds);
	if (!ret)
		ret = devm_register_sys_off_handler(dev, SYS_OFF_MODE_POWER_OFF_PREPARE,
						    SYS_OFF_PRIO_DEFAULT,
						    ds923_power_off_prepare, ds);
	if (ret)
		dev_warn(dev, "sys-off handlers: %d\n", ret);
	ds->emergency_nb.notifier_call = ds923_emergency_restart;
	if (!register_emergency_restart_prep_notifier(&ds->emergency_nb))
		devm_add_action_or_reset(dev, ds923_emergency_unregister, &ds->emergency_nb);

	ds923_spinup(ds);

	hwmon = devm_hwmon_device_register_with_info(dev, "synology", ds, &ds923_hwmon_chip, NULL);
	if (IS_ERR(hwmon))
		dev_warn(dev, "hwmon: %pe\n", hwmon);

	INIT_DELAYED_WORK(&ds->poll, ds923_poll);
	queue_delayed_work(system_freezable_wq, &ds->poll, msecs_to_jiffies(ds923_poll_ms()));
	dev_info(dev, "Synology DS923+ platform ready\n");
	return 0;
}

static void ds923_remove(struct platform_device *pdev)
{
	struct ds923 *ds = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&ds->poll);
	/* Bay and USB power are left as they are: unbinding must not cut the disks. */
}

static void ds923_shutdown(struct platform_device *pdev)
{
	struct ds923 *ds = platform_get_drvdata(pdev);
	int bay;

	cancel_delayed_work_sync(&ds->poll);
	if (system_state != SYSTEM_POWER_OFF)
		return;
	/* The disks are already stopped by sd_shutdown(). */
	for (bay = 0; bay < NUM_BAYS; bay++) {
		if (!bay_powered(ds, bay))
			continue;
		bay_set_power(ds, bay, false);
		msleep(500);
	}
}

static struct platform_driver ds923_driver = {
	.driver = {
		.name = DRV_NAME,
		.dev_groups = ds923_groups,
		/* the spin-up sleeps: let other built-in drivers go on meanwhile */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = ds923_probe,
	.remove = ds923_remove,
	.shutdown = ds923_shutdown,
};

static const struct dmi_system_id ds923_dmi[] = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Synology Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "DS923+"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, ds923_dmi);

/* Built in only (Kconfig bool): there is no exit path. */
static int __init ds923_init(void)
{
	struct platform_device *pdev;
	int ret;

	if (!dmi_check_system(ds923_dmi) && !force)
		return -ENODEV;

	/*
	 * The notifier stays registered for good, so the clients come back
	 * whenever the adapter does. The first scan is waited for, so that
	 * the RTC exists (as rtc0: there is no other RTC driver on this board)
	 * before rtc_hctosys runs at late_initcall.
	 */
	ret = bus_register_notifier(&i2c_bus_type, &ds923_i2c_nb);
	if (ret)
		pr_warn(DRV_NAME ": i2c bus notifier: %d\n", ret);
	schedule_work(&ds923_i2c_work);
	flush_work(&ds923_i2c_work);

	gpiod_add_lookup_table(&ds923_gpios);
	ret = platform_driver_register(&ds923_driver);
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
	platform_driver_unregister(&ds923_driver);
err_table:
	gpiod_remove_lookup_table(&ds923_gpios);
	return ret;
}
module_init(ds923_init);

MODULE_AUTHOR("Marco Ziliani <508111+rylos@users.noreply.github.com>");
MODULE_DESCRIPTION("Synology DS923+ platform: bays, USB VBUS, fans, RTC/LED devices, microcontroller shutdown");
MODULE_LICENSE("GPL");
