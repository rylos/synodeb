// SPDX-License-Identifier: GPL-2.0
/*
 * Synology ATmega1608 disk LED controller (DS923+, DS723+, DS1522+ ...).
 *
 * Register map (same usage as DSM 7.4 on the DS923+):
 *   0x00          chip probe (read only)
 *   0x01          disk LED mask: 1 (power-on value) keeps every LED dark whatever the
 *                 mode registers say, 0 lets them show. DSM clears it from its
 *                 platform code once the disk LEDs are set up, not from its LED driver
 *   0x02 / 0x03   PWM channel A: 0x02 = 30 (as DSM), 0x03 = brightness
 *   0x04 / 0x05   PWM channel B (unused by DSM)
 *   0x06..0x0b    LED mode, 2 bits per LED, 4 LEDs per register:
 *                 reg = 6 + led / 4, shift = 2 * (led % 4)
 *                 0 = off, 1 = on, 2 = PWM A, 3 = PWM B
 *
 * DS923+ wiring (model.dtb): led 2n = bay n+1 green, led 2n+1 = bay n+1 orange.
 *
 * Brightness: 0 = off and 255 = on are per LED; every value in between uses
 * PWM channel A, which all LEDs share. Writing an intermediate value to one
 * LED therefore dims every LED that is dimmed at that moment to the same
 * level, and their brightness reads back that common level. Use one level
 * for all LEDs (synohwd led_brightness) to keep them consistent.
 */

#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>

#define NUM_LEDS		8
#define REG_ID			0x00
#define REG_MASK		0x01
#define REG_PWM_A_CFG		0x02
#define REG_PWM_A_DUTY		0x03
#define REG_MODE(led)		(0x06 + (led) / 4)
#define MODE_SHIFT(led)		(2 * ((led) % 4))
#define MODE_OFF		0
#define MODE_ON			1
#define MODE_PWM_A		2
#define PWM_A_CFG		30

struct atmega_led {
	struct led_classdev cdev;
	struct atmega1608 *chip;
	int index;
	char name[32];
};

struct atmega1608 {
	struct i2c_client *client;
	struct mutex lock;
	struct atmega_led leds[NUM_LEDS];
};

static int atmega_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct atmega_led *led = container_of(cdev, struct atmega_led, cdev);
	struct atmega1608 *chip = led->chip;
	int reg = REG_MODE(led->index), shift = MODE_SHIFT(led->index);
	int mode, cur, ret;

	if (value == LED_OFF)
		mode = MODE_OFF;
	else if (value >= LED_FULL)
		mode = MODE_ON;
	else
		mode = MODE_PWM_A;

	mutex_lock(&chip->lock);
	if (mode == MODE_PWM_A) {
		ret = i2c_smbus_write_byte_data(chip->client, REG_PWM_A_CFG, PWM_A_CFG);
		if (!ret)
			ret = i2c_smbus_write_byte_data(chip->client, REG_PWM_A_DUTY, value);
		if (ret)
			goto out;
	}
	cur = i2c_smbus_read_byte_data(chip->client, reg);
	if (cur < 0) {
		ret = cur;
		goto out;
	}
	ret = i2c_smbus_write_byte_data(chip->client, reg,
					(cur & ~(3 << shift)) | (mode << shift));
out:
	mutex_unlock(&chip->lock);
	return ret;
}

static enum led_brightness atmega_get(struct led_classdev *cdev)
{
	struct atmega_led *led = container_of(cdev, struct atmega_led, cdev);
	int cur = i2c_smbus_read_byte_data(led->chip->client, REG_MODE(led->index));

	if (cur < 0)
		return LED_OFF;
	switch ((cur >> MODE_SHIFT(led->index)) & 3) {
	case MODE_OFF:
		return LED_OFF;
	case MODE_ON:
		return LED_FULL;
	case MODE_PWM_A:
		/* the shared level, whichever LED set it last */
		cur = i2c_smbus_read_byte_data(led->chip->client, REG_PWM_A_DUTY);
		if (cur > LED_OFF && cur < LED_FULL)
			return cur;
		fallthrough;
	default:
		return cdev->brightness ? cdev->brightness : LED_HALF;
	}
}

static int atmega_probe(struct i2c_client *client)
{
	struct atmega1608 *chip;
	int i, id, ret;

	id = i2c_smbus_read_byte_data(client, REG_ID);
	if (id < 0)
		return dev_err_probe(&client->dev, id, "chip not responding\n");

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->client = client;
	mutex_init(&chip->lock);
	i2c_set_clientdata(client, chip);

	for (i = 0; i < NUM_LEDS; i++) {
		struct atmega_led *led = &chip->leds[i];

		led->chip = chip;
		led->index = i;
		snprintf(led->name, sizeof(led->name), "syno:%s:disk%d",
			 i % 2 ? "orange" : "green", i / 2 + 1);
		led->cdev.name = led->name;
		led->cdev.max_brightness = LED_FULL;
		led->cdev.brightness_set_blocking = atmega_set;
		led->cdev.brightness_get = atmega_get;
		led->cdev.flags = LED_RETAIN_AT_SHUTDOWN;
		ret = devm_led_classdev_register(&client->dev, &led->cdev);
		if (ret)
			return dev_err_probe(&client->dev, ret, "register %s\n", led->name);
	}
	/* As DSM: LEDs registered (all off), then the mask is lifted */
	ret = i2c_smbus_write_byte_data(client, REG_MASK, 0);
	if (ret)
		dev_warn(&client->dev, "LED mask not cleared: %d\n", ret);
	dev_info(&client->dev, "ATmega1608 LED controller, id 0x%02x\n", id);
	return 0;
}

static const struct i2c_device_id atmega_id[] = {
	{ "atmega1608" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, atmega_id);

static struct i2c_driver atmega_driver = {
	.driver = { .name = "leds-atmega1608" },
	.probe = atmega_probe,
	.id_table = atmega_id,
};
module_i2c_driver(atmega_driver);

MODULE_AUTHOR("Marco Ziliani <508111+rylos@users.noreply.github.com>");
MODULE_DESCRIPTION("Synology ATmega1608 disk LED controller");
MODULE_LICENSE("GPL");
