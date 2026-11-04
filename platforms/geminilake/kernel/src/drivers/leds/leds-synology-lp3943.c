// SPDX-License-Identifier: GPL-2.0
/*
 * Disk LEDs of Synology NAS with a TI LP3943 LED driver (DS920+ and other
 * Gemini Lake models).
 *
 * The BIOS describes the LP3943 in ACPI as "LED3943" (on the SMBus of the
 * DS920+, address 0x60). Its 16 open-drain outputs each sink one LED; the
 * mode of output n is 2 bits in register LS0 + n / 4 (00 off, 01 on, 10/11
 * blink at PWM0/PWM1). The wiring is the one of DSM's model.dtb: output 2n is
 * the green LED of bay n + 1, output 2n + 1 its orange LED.
 *
 * Only on/off is used; blinking is left to the LED triggers (synohwd uses
 * "ataN" for disk activity and "timer" for a failed disk). The mfd lp3943
 * driver binds the same chip by device tree, not by ACPI, so the two never
 * meet.
 */

#include <linux/acpi.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>

#define LP3943_LS0	0x06		/* LS0..LS3: outputs 0-3 .. 12-15 */
#define LP3943_OUTPUTS	16
#define LP3943_OFF	0x0
#define LP3943_ON	0x1

static unsigned int bays = 4;
module_param(bays, uint, 0444);
MODULE_PARM_DESC(bays, "Number of bays (two LEDs each, at most 8)");

struct lp3943_led {
	struct led_classdev cdev;
	struct lp3943 *chip;
	unsigned int output;
	char name[32];
};

struct lp3943 {
	struct i2c_client *client;
	struct mutex lock;		/* read-modify-write of the LS registers */
	struct lp3943_led led[LP3943_OUTPUTS];
};

static int lp3943_set_mode(struct lp3943 *chip, unsigned int output, u8 mode)
{
	u8 reg = LP3943_LS0 + output / 4, shift = (output % 4) * 2;
	int val, ret;

	mutex_lock(&chip->lock);
	val = i2c_smbus_read_byte_data(chip->client, reg);
	if (val < 0) {
		ret = val;
	} else {
		val = (val & ~(0x3 << shift)) | (mode << shift);
		ret = i2c_smbus_write_byte_data(chip->client, reg, val);
	}
	mutex_unlock(&chip->lock);
	return ret;
}

static int lp3943_brightness_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct lp3943_led *led = container_of(cdev, struct lp3943_led, cdev);

	return lp3943_set_mode(led->chip, led->output, value ? LP3943_ON : LP3943_OFF);
}

static enum led_brightness lp3943_brightness_get(struct led_classdev *cdev)
{
	struct lp3943_led *led = container_of(cdev, struct lp3943_led, cdev);
	int val = i2c_smbus_read_byte_data(led->chip->client, LP3943_LS0 + led->output / 4);

	if (val < 0)
		return LED_OFF;
	return ((val >> ((led->output % 4) * 2)) & 0x3) != LP3943_OFF;
}

static int lp3943_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct lp3943 *chip;
	unsigned int i, n = min(bays, LP3943_OUTPUTS / 2) * 2;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;
	/* the chip answers: catch a wrong address before registering anything */
	ret = i2c_smbus_read_byte_data(client, LP3943_LS0);
	if (ret < 0)
		return dev_err_probe(dev, ret, "no LP3943 at 0x%02x\n", client->addr);

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->client = client;
	ret = devm_mutex_init(dev, &chip->lock);
	if (ret)
		return ret;

	for (i = 0; i < n; i++) {
		struct lp3943_led *led = &chip->led[i];

		led->chip = chip;
		led->output = i;
		snprintf(led->name, sizeof(led->name), "syno:%s:disk%u",
			 i % 2 ? "orange" : "green", i / 2 + 1);
		led->cdev.name = led->name;
		led->cdev.max_brightness = 1;
		led->cdev.brightness_set_blocking = lp3943_brightness_set;
		led->cdev.brightness_get = lp3943_brightness_get;
		led->cdev.brightness = lp3943_brightness_get(&led->cdev);
		ret = devm_led_classdev_register(dev, &led->cdev);
		if (ret)
			return dev_err_probe(dev, ret, "register %s\n", led->name);
	}
	return 0;
}

static const struct acpi_device_id lp3943_acpi_ids[] = {
	{ "LED3943" },
	{ }
};
MODULE_DEVICE_TABLE(acpi, lp3943_acpi_ids);

static struct i2c_driver lp3943_driver = {
	.driver = {
		.name = "leds-synology-lp3943",
		.acpi_match_table = lp3943_acpi_ids,
	},
	.probe = lp3943_probe,
};
module_i2c_driver(lp3943_driver);

MODULE_AUTHOR("Marco Ziliani <508111+rylos@users.noreply.github.com>");
MODULE_DESCRIPTION("Disk LEDs of Synology NAS on a TI LP3943 (ACPI LED3943)");
MODULE_LICENSE("GPL");
