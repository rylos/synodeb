// SPDX-License-Identifier: GPL-2.0
/*
 * Synology expansion units built on Marvell 88SM9705 port multipliers.
 *
 * Fans, fan alarms and bay LEDs of these units hang off GPIO pins of their
 * PMPs. The pins carry a paged 8-bit register file, reached with PMP
 * READ/WRITE to vendor registers 0x3a0/0x3a4/0x3b0 of the PMP control port
 * (encoding and write policy: synology_eunit_9705.h). Register access goes
 * through sata_pmp_vendor_call(), from a quiet EH round of the port.
 *
 * Pages, measured on a DX1215:
 *   0  bits 7:5 EMID (0 = unit master), bits 4:0 unit ID (0x13 on all four
 *      PMPs, EMID 0-3)
 *   1  per PMP: bits 0-2 orange fault LED of fan-out ports 0-2, bit 5 a
 *      pulse line (never set), bit 6 all disk LEDs off
 *   2  master: bit 0/1 fan 1/2 alarm
 *   3  master: bit 0 rear DEFAULT/MANUAL switch
 *   4  master: bits 1:0 fan level (00 full, 01 middle, 10 low, 11 stop),
 *      bit 4 a power-switch pulse line (never set)
 * DX1215 bay = 3 * EMID + fan-out port + 1 (checked tray by tray).
 *
 * Each 88SM9705 unit model has its own unit ID in page 0 bits 4:0 of its
 * master PMP, and all of them use the same GPIO word encoding. Only the
 * DX1215 row is confirmed on hardware; every row is used, and the log says
 * which row and whether it is confirmed. A PMP whose unit ID is not in the
 * table gets read-only sysfs files and nothing else.
 *
 * Binding: the driver looks at AHCI ports of supported Synology hosts only
 * (DMI vendor "Synology Inc."; the DS2415+, whose BIOS has placeholder DMI
 * strings, by DMI and PCI topology), plus the ports named with the "ports"
 * parameter. On those ports it acts only where a 1b4b:9705 PMP is attached,
 * so the internal bays that share a controller with an eSATA port are never
 * touched, and neither is a 88SM9705 on any other machine (generic eSATA
 * cards and enclosures use the same chip).
 *
 * Interfaces per unit: hwmon "synology_<model>" (pwm1: 255 full, 128
 * middle, 64 low, a write above 128 gives full, above 64 middle, else low;
 * pwm1_enable 1; fanN_alarm) with unit_name, unit_id, confirmed, pmps
 * ("ataN EMID" lines) and pages (raw page bytes, root only); LEDs
 * syno-eunitK:orange:diskN.
 */

#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/idr.h>
#include <linux/jiffies.h>
#include <linux/leds.h>
#include <linux/libata.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "synology_eunit_9705.h"

#if !IS_ENABLED(CONFIG_SATA_PMP_VENDOR_EH)
#error "needs a kernel with the PMP vendor-register API (CONFIG_SATA_PMP_VENDOR_EH)"
#endif

#define DRV_NAME		"synology-eunit-9705"
#define EU_MAX_PMPS		4
#define EU_MAX_PORTS		32	/* ATA ports per controller */
#define EU_MAX_CTRLS		8
#define EU_SCAN_S		30
#define EU_MASTER_PAGES		(BIT(0) | BIT(2) | BIT(4))
#define EU_ALL_PAGES		(BIT(EU_PAGES) - 1)

static const u8 eu_level_pwm[4] = { 255, 128, 64, 0 };

static bool disk_leds = true;
module_param(disk_leds, bool, 0444);
MODULE_PARM_DESC(disk_leds, "Turn the disk LEDs of a unit on when it is found (page 1 bit 6)");

static unsigned int refresh_s = 10;
module_param(refresh_s, uint, 0644);
MODULE_PARM_DESC(refresh_s, "Max age in s of the fan values before a read asks the unit again");

static char *ports;
module_param(ports, charp, 0444);
MODULE_PARM_DESC(ports, "More AHCI ports to look at, any host: DDDD:BB:DD.F/port[,...]");

struct eu_ctrl;
struct eu_unit;

struct eu_port {
	struct ata_port *ap;
	struct eu_unit *unit;		/* NULL: not part of a unit */
	bool p0_valid;			/* page 0 read since the PMP attached */
	u8 p0;
};

struct eu_led {
	struct led_classdev cdev;
	struct eu_unit *unit;
	unsigned int bay;		/* 0-based */
	char name[40];
};

struct eu_unit {
	struct list_head node;
	struct eu_ctrl *ctrl;
	const struct eu_type *type;	/* NULL: unit ID not in the table */
	u8 id;
	int index;			/* syno-eunit<index> */
	int npmps;
	int pmp[EU_MAX_PMPS];		/* port per EMID, -1: not seen */
	bool gone;
	u8 mpage[EU_PAGES];		/* master pages, read at mstamp */
	bool mvalid;
	unsigned long mstamp;
	struct device *hwmon;
	char hwmon_name[24];
	unsigned int nleds;
	struct eu_led led[EU_MAX_PMPS * EU_MAX_LOCAL];
};

struct eu_ctrl {
	struct device *dev;
	struct pci_dev *pdev;
	struct ata_host *host;
	u32 portmask;
	struct mutex lock;		/* cycles, ports, units, unit caches */
	struct eu_port port[EU_MAX_PORTS];
	struct list_head units;
	struct delayed_work work;
};

/* a controller to look at, set up at module init */
struct eu_cand {
	struct pci_dev *pdev;
	u32 portmask;
	struct platform_device *plat;
};

static struct eu_cand eu_cands[EU_MAX_CTRLS];
static int eu_ncands;
static DEFINE_IDA(eu_ida);

/* --------------------------------------------------------- register cycle */

/* One request, run by eu_cycle_fn() in a quiet EH round of one PMP port. */
struct eu_req {
	struct eu_guard g;		/* what eu_write() decides on */
	unsigned int pages;		/* bit n: read page n */
	int wpage;			/* -1, 1 or 4 */
	u8 wmask, wval;			/* page bits to change and their value */
	u32 out, en;			/* 0x3a0/0x3a4 found at the start */
};

static void eu_req_init(struct eu_req *r, const struct eu_unit *u,
			unsigned int pages)
{
	memset(r, 0, sizeof(*r));
	r->pages = pages;
	r->wpage = -1;
	r->g.armed = -1;
	if (u && u->type) {
		r->g.writable = true;
		r->g.id = u->type->id;
		r->g.p1_mask = eu_type_p1_mask(u->type);
	}
}

/* The only PMP write of the driver. */
static unsigned int eu_write(struct ata_port *ap, struct eu_req *r, u16 reg,
			     u32 val)
{
	if (!eu_write_ok(&r->g, reg, val)) {
		WARN_ONCE(1, DRV_NAME ": refused PMP write reg 0x%x val 0x%x\n",
			  reg, val);
		return AC_ERR_INVALID;
	}
	return sata_pmp_vendor_write(ap, reg, val);
}

/* output enable all-out, then 0x3a0; 5 ms for the unit */
static unsigned int eu_out(struct ata_port *ap, struct eu_req *r, u32 val)
{
	unsigned int err = eu_write(ap, r, EU_REG_EN, EU_EN_ALL_OUT);

	if (!err)
		err = eu_write(ap, r, EU_REG_OUT, val);
	usleep_range(5000, 6000);
	return err;
}

/* page select, read strobe low, read 0x3b0, read strobe high */
static unsigned int eu_read_page(struct ata_port *ap, struct eu_req *r, int pg)
{
	unsigned int err;
	u32 raw;

	err = eu_out(ap, r, EU_R_SEL(pg, 0));
	if (!err)
		err = eu_out(ap, r, EU_R_SEL(pg, 1));
	if (!err)
		err = eu_write(ap, r, EU_REG_EN, EU_EN_READ);
	if (!err)
		err = sata_pmp_vendor_read(ap, EU_REG_IN, &raw);
	if (!err)
		err = eu_out(ap, r, EU_R_SEL(pg, 0));
	if (!err) {
		r->g.page[pg] = EU_DEC(raw);
		r->g.valid |= BIT(pg);
	}
	return err;
}

/* 0x3a0 and 0x3a4 back to what they were at the start, read back */
static unsigned int eu_restore(struct ata_port *ap, struct eu_req *r)
{
	unsigned int err;
	u32 out, en;

	err = eu_out(ap, r, r->out);
	if (!err)
		err = eu_write(ap, r, EU_REG_EN, r->en);
	if (!err)
		err = sata_pmp_vendor_read(ap, EU_REG_OUT, &out);
	if (!err)
		err = sata_pmp_vendor_read(ap, EU_REG_EN, &en);
	if (!err && (out != r->out || en != r->en))
		err = AC_ERR_OTHER;
	return err;
}

/* data word, write strobe low, data word; then read back */
static int eu_write_page(struct ata_port *ap, struct eu_req *r)
{
	struct eu_guard *g = &r->g;
	int p = r->wpage, i;
	unsigned int err = 0;

	g->cur = g->page[p];
	if (!g->writable || EU_P0_ID(g->page[0]) != g->id)
		return -ENODEV;
	if (p == 4) {
		if (EU_P0_EMID(g->page[0]) || (g->cur & EU_P4_POWER_PULSE) ||
		    (r->wval & EU_P4_FAN) == EU_FAN_STOP)
			return -EPERM;
		g->new = eu_p4_new(g->cur, r->wval);
	} else {
		if (g->cur & EU_P1_BUZZER)
			return -EPERM;
		g->new = eu_p1_new(g->cur, g->p1_mask, r->wmask, r->wval);
	}
	if (g->new == g->cur)
		return 0;

	g->armed = p;
	for (i = 0; i < 3 && !err; i++)
		err = eu_out(ap, r, EU_R_DATA(p, g->new, i == 1));
	g->armed = -1;
	if (err || ata_port_is_frozen(ap))
		return -EIO;
	err = eu_restore(ap, r);
	if (!err)
		err = eu_read_page(ap, r, p);
	if (err)
		return -EIO;
	return g->page[p] == g->new ? 0 : -EIO;
}

static bool eu_is_9705(struct ata_port *ap)
{
	u32 *gscr = ap->link.device->gscr;

	return sata_pmp_attached(ap) &&
	       sata_pmp_gscr_vendor(gscr) == PCI_VENDOR_ID_MARVELL_EXT &&
	       sata_pmp_gscr_devid(gscr) == 0x9705;
}

static int eu_cycle_fn(struct ata_port *ap, void *data)
{
	struct eu_req *r = data;
	unsigned int pages = r->pages, err;
	int pg, rc = 0;
	u32 pol;

	r->g.armed = -1;
	r->g.valid = 0;
	if (!eu_is_9705(ap))
		return -ENODEV;
	err = sata_pmp_vendor_read(ap, EU_REG_OUT, &r->out);
	if (!err)
		err = sata_pmp_vendor_read(ap, EU_REG_EN, &r->en);
	if (err)
		return -EIO;
	/* input polarity 0xfffff; otherwise inputs read inverted */
	err = sata_pmp_vendor_read(ap, EU_REG_POL, &pol);
	if (!err && pol != EU_POL_ALL) {
		err = eu_write(ap, r, EU_REG_POL, EU_POL_ALL);
		if (!err)
			err = sata_pmp_vendor_read(ap, EU_REG_POL, &pol);
		if (!err && pol != EU_POL_ALL)
			err = AC_ERR_OTHER;
	}
	if (err)
		return -EIO;
	/* never set up since power-on: end in the set-up idle state instead */
	if (r->out == 0 && r->en == EU_EN_POWERON) {
		r->out = EU_R_IDLE;
		r->en = EU_EN_ALL_OUT;
	}
	/* not a state we could put back: leave the PMP alone */
	if (!EU_R_SELECT_OK(r->out) ||
	    (r->en != EU_EN_ALL_OUT && r->en != EU_EN_READ))
		return -EIO;

	/* a write needs page 0 (unit ID, EMID) and the page itself from this round */
	if (r->wpage >= 0)
		pages |= BIT(0) | BIT(r->wpage);
	for (pg = 0; pg < EU_PAGES && !err; pg++)
		if (pages & BIT(pg))
			err = eu_read_page(ap, r, pg);
	if (!err && r->wpage >= 0)
		rc = eu_write_page(ap, r);
	if (ata_port_is_frozen(ap))
		return -EIO;
	if (eu_restore(ap, r) || err)
		return -EIO;
	return rc;
}

static int eu_cycle(struct ata_port *ap, struct eu_req *r)
{
	return sata_pmp_vendor_call(ap, eu_cycle_fn, r);
}

/* ------------------------------------------------------------------ unit */

static struct ata_port *eu_unit_ap(struct eu_unit *u, int emid)
{
	int i = u->pmp[emid];

	return i < 0 ? NULL : u->ctrl->port[i].ap;
}

/* caller holds lock: master pages 0, 2, 4 no older than refresh_s */
static int eu_master_fresh(struct eu_unit *u)
{
	struct eu_req r;
	int ret;

	if (u->gone || !u->type)
		return -ENODEV;
	if (u->mvalid &&
	    time_before(jiffies, u->mstamp + max(refresh_s, 1U) * HZ))
		return 0;
	eu_req_init(&r, u, EU_MASTER_PAGES);
	ret = eu_cycle(eu_unit_ap(u, 0), &r);
	if (!ret && r.g.page[0] != u->id)
		ret = -ENODEV;
	u->mvalid = !ret;
	if (ret)
		return ret;
	memcpy(u->mpage, r.g.page, sizeof(u->mpage));
	u->mstamp = jiffies;
	return 0;
}

/* level write on the master; on any failure full once */
static int eu_set_level(struct eu_unit *u, unsigned int code)
{
	struct eu_ctrl *c = u->ctrl;
	struct eu_req r;
	int ret;

	if (code > EU_FAN_LOW)
		return -EINVAL;
	mutex_lock(&c->lock);
	if (u->gone || !u->type) {
		mutex_unlock(&c->lock);
		return -ENODEV;
	}
	eu_req_init(&r, u, EU_MASTER_PAGES);
	r.wpage = 4;
	r.wmask = EU_P4_FAN;
	r.wval = code;
	ret = eu_cycle(eu_unit_ap(u, 0), &r);
	if (ret && ret != -ENODEV && code != EU_FAN_FULL) {
		struct eu_req full;

		dev_warn(c->dev, "%s: fan level %u: %d, setting full\n",
			 u->type->name, code, ret);
		eu_req_init(&full, u, EU_MASTER_PAGES);
		full.wpage = 4;
		full.wmask = EU_P4_FAN;
		full.wval = EU_FAN_FULL;
		if (!eu_cycle(eu_unit_ap(u, 0), &full))
			r = full;
	}
	u->mvalid = false;
	if ((r.g.valid & EU_MASTER_PAGES) == EU_MASTER_PAGES &&
	    r.g.page[0] == u->id) {
		memcpy(u->mpage, r.g.page, sizeof(u->mpage));
		u->mstamp = jiffies;
		u->mvalid = true;
	}
	mutex_unlock(&c->lock);
	return ret;
}

static int eu_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	struct eu_unit *u = dev_get_drvdata(dev);
	int ret;

	if (type == hwmon_pwm && attr == hwmon_pwm_enable) {
		*val = 1;
		return 0;
	}
	mutex_lock(&u->ctrl->lock);
	ret = eu_master_fresh(u);
	if (ret)
		ret = -ENODATA;
	else if (type == hwmon_fan)
		*val = (u->mpage[2] >> channel) & 1;
	else
		*val = eu_level_pwm[u->mpage[4] & EU_P4_FAN];
	mutex_unlock(&u->ctrl->lock);
	return ret;
}

static int eu_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long val)
{
	struct eu_unit *u = dev_get_drvdata(dev);

	if (attr == hwmon_pwm_enable)
		return val == 1 ? 0 : -EINVAL;
	if (val < 0 || val > 255)
		return -EINVAL;
	return eu_set_level(u, val > 128 ? EU_FAN_FULL :
			       val > 64 ? EU_FAN_MIDDLE : EU_FAN_LOW);
}

static umode_t eu_hwmon_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	const struct eu_unit *u = data;

	if (!u->type)
		return 0;
	if (type == hwmon_fan && attr == hwmon_fan_alarm &&
	    channel < u->type->fans)
		return 0444;
	if (type == hwmon_pwm &&
	    (attr == hwmon_pwm_input || attr == hwmon_pwm_enable))
		return 0644;
	return 0;
}

static const struct hwmon_ops eu_hwmon_ops = {
	.is_visible = eu_hwmon_visible,
	.read = eu_hwmon_read,
	.write = eu_hwmon_write,
};

static const struct hwmon_channel_info * const eu_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_ALARM, HWMON_F_ALARM, HWMON_F_ALARM,
			   HWMON_F_ALARM),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_chip_info eu_hwmon_chip = {
	.ops = &eu_hwmon_ops,
	.info = eu_hwmon_info,
};

static int eu_led_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct eu_led *led = container_of(cdev, struct eu_led, cdev);
	struct eu_unit *u = led->unit;
	unsigned int per = u->type->bays / u->type->pmps;
	int emid = led->bay / per, ret = -ENODEV;
	u8 bit = BIT(u->type->led_bit[led->bay % per]);
	struct ata_port *ap;
	struct eu_req r;

	mutex_lock(&u->ctrl->lock);
	ap = u->gone ? NULL : eu_unit_ap(u, emid);
	if (ap) {
		eu_req_init(&r, u, 0);
		r.wpage = 1;
		r.wmask = bit;
		r.wval = value ? bit : 0;
		ret = eu_cycle(ap, &r);
	}
	mutex_unlock(&u->ctrl->lock);
	return ret;
}

/* ----------------------------------------------------------------- sysfs */

static ssize_t unit_name_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct eu_unit *u = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", u->type ? u->type->name : "unknown");
}
static DEVICE_ATTR_RO(unit_name);

static ssize_t unit_id_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct eu_unit *u = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%02x\n", EU_P0_ID(u->id));
}
static DEVICE_ATTR_RO(unit_id);

static ssize_t confirmed_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct eu_unit *u = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", u->type && u->type->confirmed);
}
static DEVICE_ATTR_RO(confirmed);

static ssize_t pmps_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct eu_unit *u = dev_get_drvdata(dev);
	struct eu_ctrl *c = u->ctrl;
	ssize_t n = 0;
	int e;

	mutex_lock(&c->lock);
	for (e = 0; e < u->npmps; e++)
		if (u->pmp[e] >= 0)
			n += sysfs_emit_at(buf, n, "ata%u %d\n",
					   c->port[u->pmp[e]].ap->print_id,
					   EU_P0_EMID(c->port[u->pmp[e]].p0));
	mutex_unlock(&c->lock);
	return n;
}
static DEVICE_ATTR_RO(pmps);

/* raw page bytes of every PMP of the unit, read now (read-only cycles) */
static ssize_t pages_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct eu_unit *u = dev_get_drvdata(dev);
	struct eu_ctrl *c = u->ctrl;
	ssize_t n = 0;
	int e, pg, ret;

	mutex_lock(&c->lock);
	for (e = 0; e < u->npmps && !u->gone; e++) {
		struct ata_port *ap = eu_unit_ap(u, e);
		struct eu_req r;

		if (!ap)
			continue;
		/* read-only: no row, so the guard allows no data word */
		eu_req_init(&r, NULL, EU_ALL_PAGES);
		ret = eu_cycle(ap, &r);
		n += sysfs_emit_at(buf, n, "ata%u", ap->print_id);
		if (ret) {
			n += sysfs_emit_at(buf, n, " error %d\n", ret);
			continue;
		}
		for (pg = 0; pg < EU_PAGES; pg++)
			n += sysfs_emit_at(buf, n, " %02x", r.g.page[pg]);
		n += sysfs_emit_at(buf, n, "\n");
	}
	mutex_unlock(&c->lock);
	return n;
}
static DEVICE_ATTR_ADMIN_RO(pages);

static struct attribute *eu_unit_attrs[] = {
	&dev_attr_unit_name.attr,
	&dev_attr_unit_id.attr,
	&dev_attr_confirmed.attr,
	&dev_attr_pmps.attr,
	&dev_attr_pages.attr,
	NULL
};
ATTRIBUTE_GROUPS(eu_unit);

/* ------------------------------------------------------------ discovery */

/* turn the disk LEDs of one PMP on (page 1 bit 6 = 0) */
static void eu_leds_on(struct eu_unit *u, int emid)
{
	struct ata_port *ap = eu_unit_ap(u, emid);
	struct eu_req r;
	int ret;

	if (!disk_leds || !u->type || !ap)
		return;
	eu_req_init(&r, u, 0);
	r.wpage = 1;
	r.wmask = EU_P1_LED_MASK;
	r.wval = 0;
	ret = eu_cycle(ap, &r);
	if (ret)
		dev_warn(u->ctrl->dev, "ata%u: disk LEDs on: %d\n",
			 ap->print_id, ret);
}

/* without the lock: the callbacks take it */
static void eu_unit_unregister(struct eu_unit *u)
{
	unsigned int i;

	for (i = 0; i < u->nleds; i++)
		led_classdev_unregister(&u->led[i].cdev);
	if (u->hwmon)
		hwmon_device_unregister(u->hwmon);
	ida_free(&eu_ida, u->index);
	kfree(u);
}

/* caller holds lock */
static struct eu_unit *eu_unit_new(struct eu_ctrl *c, int master)
{
	struct eu_port *p = &c->port[master];
	const struct eu_type *t = eu_type_lookup(EU_P0_ID(p->p0));
	struct eu_unit *u;
	unsigned int i;
	int ret, e;

	u = kzalloc(sizeof(*u), GFP_KERNEL);
	if (!u)
		return NULL;
	ret = ida_alloc_min(&eu_ida, 1, GFP_KERNEL);
	if (ret < 0) {
		kfree(u);
		return NULL;
	}
	u->index = ret;
	u->ctrl = c;
	u->type = t;
	u->id = p->p0;
	u->npmps = t ? t->pmps : 1;
	for (e = 0; e < EU_MAX_PMPS; e++)
		u->pmp[e] = -1;
	u->pmp[0] = master;
	p->unit = u;

	if (t)
		snprintf(u->hwmon_name, sizeof(u->hwmon_name), "synology_%s", t->name);
	else
		strscpy(u->hwmon_name, "synology_eunit", sizeof(u->hwmon_name));
	for (i = 0; u->hwmon_name[i]; i++)
		u->hwmon_name[i] = tolower(u->hwmon_name[i]);

	if (t)
		dev_info(c->dev, "ata%u: unit ID 0x%02x, using row %s (%s)\n",
			 p->ap->print_id, EU_P0_ID(p->p0), t->name,
			 t->confirmed ? "confirmed" : "unconfirmed, please report");
	else
		dev_info(c->dev, "ata%u: unit ID 0x%02x (EMID %u) not in the table: read-only, please report\n",
			 p->ap->print_id, EU_P0_ID(p->p0), EU_P0_EMID(p->p0));

	u->hwmon = hwmon_device_register_with_info(c->dev, u->hwmon_name, u,
						   &eu_hwmon_chip, eu_unit_groups);
	if (IS_ERR(u->hwmon)) {
		dev_warn(c->dev, "%s: hwmon: %ld\n", u->hwmon_name,
			 PTR_ERR(u->hwmon));
		u->hwmon = NULL;
	}
	if (!t)
		return u;
	for (i = 0; i < t->bays; i++) {
		struct eu_led *led = &u->led[i];

		led->unit = u;
		led->bay = i;
		snprintf(led->name, sizeof(led->name),
			 "syno-eunit%d:orange:disk%u", u->index, i + 1);
		led->cdev.name = led->name;
		led->cdev.max_brightness = 1;
		led->cdev.brightness_set_blocking = eu_led_set;
		led->cdev.flags = LED_RETAIN_AT_SHUTDOWN;
		ret = led_classdev_register(c->dev, &led->cdev);
		if (ret) {
			dev_warn(c->dev, "%s: %d\n", led->name, ret);
			break;
		}
		u->nleds++;
	}
	eu_leds_on(u, 0);
	return u;
}

/*
 * The unit a slave PMP belongs to: same unit ID, its EMID slot free; with
 * more than one, the nearest master before it in port order.
 */
static struct eu_unit *eu_slave_unit(struct eu_ctrl *c, int i)
{
	struct eu_unit *u, *best = NULL;
	int emid = EU_P0_EMID(c->port[i].p0);

	list_for_each_entry(u, &c->units, node) {
		if (u->gone || !u->type || u->id != EU_P0_ID(c->port[i].p0) ||
		    emid >= u->npmps || u->pmp[emid] >= 0)
			continue;
		if (!best || (u->pmp[0] < i && (best->pmp[0] > i || u->pmp[0] > best->pmp[0])))
			best = u;
	}
	return best;
}

/* caller holds lock: drop what went away, read page 0 of new PMPs, group */
static void eu_scan(struct eu_ctrl *c, struct list_head *dead)
{
	struct eu_unit *u, *tmp;
	int i, e;

	for (i = 0; i < EU_MAX_PORTS; i++) {
		struct eu_port *p = &c->port[i];
		struct eu_req r;

		if (!p->ap)
			continue;
		if (!eu_is_9705(p->ap)) {
			u = p->unit;
			if (u && u->pmp[0] == i) {
				u->gone = true;
			} else if (u) {
				for (e = 0; e < u->npmps; e++)
					if (u->pmp[e] == i)
						u->pmp[e] = -1;
			}
			p->unit = NULL;
			p->p0_valid = false;
			continue;
		}
		if (p->unit || p->p0_valid)
			continue;
		eu_req_init(&r, NULL, BIT(0));
		if (!eu_cycle(p->ap, &r)) {
			p->p0 = r.g.page[0];
			p->p0_valid = true;
		}
	}

	list_for_each_entry_safe(u, tmp, &c->units, node) {
		if (!u->gone)
			continue;
		dev_info(c->dev, "%s (syno-eunit%d) gone\n", u->hwmon_name, u->index);
		for (e = 0; e < u->npmps; e++) {
			if (u->pmp[e] < 0)
				continue;
			c->port[u->pmp[e]].unit = NULL;
			c->port[u->pmp[e]].p0_valid = false;
		}
		list_move(&u->node, dead);
	}

	/* masters first (and every PMP with an unknown ID: its own unit) */
	for (i = 0; i < EU_MAX_PORTS; i++) {
		struct eu_port *p = &c->port[i];

		if (!p->ap || p->unit || !p->p0_valid)
			continue;
		if (EU_P0_EMID(p->p0) && eu_type_lookup(EU_P0_ID(p->p0)))
			continue;
		u = eu_unit_new(c, i);
		if (u)
			list_add_tail(&u->node, &c->units);
	}
	for (i = 0; i < EU_MAX_PORTS; i++) {
		struct eu_port *p = &c->port[i];

		if (!p->ap || p->unit || !p->p0_valid)
			continue;
		u = eu_slave_unit(c, i);
		if (!u)
			continue;
		e = EU_P0_EMID(p->p0);
		u->pmp[e] = i;
		p->unit = u;
		dev_info(c->dev, "ata%u: %s EMID %d\n", p->ap->print_id,
			 u->type->name, e);
		eu_leds_on(u, e);
	}
}

static void eu_work_fn(struct work_struct *work)
{
	struct eu_ctrl *c = container_of(to_delayed_work(work), struct eu_ctrl, work);
	struct eu_unit *u, *tmp;
	LIST_HEAD(dead);

	mutex_lock(&c->lock);
	eu_scan(c, &dead);
	mutex_unlock(&c->lock);
	list_for_each_entry_safe(u, tmp, &dead, node) {
		list_del(&u->node);
		eu_unit_unregister(u);
	}
	schedule_delayed_work(&c->work, EU_SCAN_S * HZ);
}

/* ---------------------------------------------------------------- probe */

static int eu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct eu_cand *cand;
	struct eu_ctrl *c;
	struct ata_host *host;
	unsigned int i;

	if (pdev->id < 0 || pdev->id >= eu_ncands)
		return -ENODEV;
	cand = &eu_cands[pdev->id];
	/* ahci's drvdata is its ata_host; the device link orders us after it */
	if (!cand->pdev->dev.driver ||
	    strcmp(cand->pdev->dev.driver->name, "ahci"))
		return dev_err_probe(dev, -ENODEV, "%s not bound to ahci\n",
				     pci_name(cand->pdev));
	host = pci_get_drvdata(cand->pdev);
	if (!host)
		return -ENODEV;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;
	c->pdev = cand->pdev;
	c->host = host;
	c->portmask = cand->portmask;
	mutex_init(&c->lock);
	INIT_LIST_HEAD(&c->units);
	INIT_DELAYED_WORK(&c->work, eu_work_fn);
	for (i = 0; i < host->n_ports && i < EU_MAX_PORTS; i++)
		if (c->portmask & BIT(i))
			c->port[i].ap = host->ports[i];
	platform_set_drvdata(pdev, c);
	/* the first look sends ATA commands: not from probe */
	schedule_delayed_work(&c->work, 0);
	dev_info(dev, "looking at %s ports 0x%x\n", pci_name(c->pdev),
		 c->portmask & (u32)(BIT_ULL(host->n_ports) - 1));
	return 0;
}

static void eu_remove(struct platform_device *pdev)
{
	struct eu_ctrl *c = platform_get_drvdata(pdev);
	struct eu_unit *u, *tmp;
	LIST_HEAD(dead);

	cancel_delayed_work_sync(&c->work);
	mutex_lock(&c->lock);
	list_for_each_entry(u, &c->units, node)
		u->gone = true;
	list_splice_init(&c->units, &dead);
	mutex_unlock(&c->lock);
	/* the fan level and LED states stay as they are */
	list_for_each_entry_safe(u, tmp, &dead, node) {
		list_del(&u->node);
		eu_unit_unregister(u);
	}
}

static int eu_resume(struct device *dev)
{
	struct eu_ctrl *c = dev_get_drvdata(dev);
	struct eu_unit *u;

	mutex_lock(&c->lock);
	list_for_each_entry(u, &c->units, node)
		u->mvalid = false;
	mutex_unlock(&c->lock);
	mod_delayed_work(system_wq, &c->work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(eu_pm, NULL, eu_resume);

static struct platform_driver eu_driver = {
	.driver = {
		.name = DRV_NAME,
		.pm = pm_sleep_ptr(&eu_pm),
	},
	.probe = eu_probe,
	.remove = eu_remove,
};

/* ----------------------------------------------------------------- hosts */

/* Synology hosts with a DMI vendor string of their own (e.g. DS923+) */
static const struct dmi_system_id eu_dmi_synology[] = {
	{ .matches = { DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Synology Inc.") } },
	{ }
};

/*
 * DS2415+: the BIOS (Insyde, also used by other Avoton boards) has
 * placeholder strings, so the PCI topology is checked as well: Avoton PCU
 * 8086:1f38 at 00:1f.0, exactly three 1b4b:9215 (internal bays) and one
 * 1b4b:9235, whose four ports go to the expansion unit.
 */
static const struct dmi_system_id eu_dmi_ds2415p[] = {
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Insyde"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "MohonPeak"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "Type2 - Board Product Name1"),
		},
	},
	{ }
};

static const struct dmi_system_id eu_dmi_all[] = {
	{ .matches = { DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Synology Inc.") } },
	{
		.matches = {
			DMI_EXACT_MATCH(DMI_SYS_VENDOR, "Insyde"),
			DMI_EXACT_MATCH(DMI_PRODUCT_NAME, "MohonPeak"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "Type2 - Board Product Name1"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, eu_dmi_all);

/* takes the reference of @pdev */
static int eu_add(struct pci_dev *pdev, u32 mask)
{
	int i;

	for (i = 0; i < eu_ncands; i++) {
		if (eu_cands[i].pdev == pdev) {
			eu_cands[i].portmask |= mask;
			pci_dev_put(pdev);
			return 0;
		}
	}
	if (eu_ncands == EU_MAX_CTRLS) {
		pci_dev_put(pdev);
		return -ENOSPC;
	}
	eu_cands[eu_ncands].pdev = pdev;
	eu_cands[eu_ncands].portmask = mask;
	eu_ncands++;
	return 0;
}

static void eu_put_cands(void)
{
	int i;

	for (i = 0; i < eu_ncands; i++)
		pci_dev_put(eu_cands[i].pdev);
	eu_ncands = 0;
}

static void eu_find_ds2415p(void)
{
	struct pci_dev *lpc, *p = NULL, *exp = NULL;
	int n = 0;

	lpc = pci_get_domain_bus_and_slot(0, 0, PCI_DEVFN(0x1f, 0));
	if (!lpc || lpc->vendor != PCI_VENDOR_ID_INTEL || lpc->device != 0x1f38) {
		pci_dev_put(lpc);
		return;
	}
	pci_dev_put(lpc);
	while ((p = pci_get_device(PCI_VENDOR_ID_MARVELL_EXT, 0x9215, p)))
		n++;
	while ((p = pci_get_device(PCI_VENDOR_ID_MARVELL_EXT, 0x9235, p))) {
		if (exp) {
			pci_dev_put(p);
			pci_dev_put(exp);
			return;
		}
		exp = pci_dev_get(p);
	}
	if (n != 3 || !exp) {
		pci_dev_put(exp);
		return;
	}
	eu_add(exp, 0xf);
}

static void eu_find_synology(void)
{
	struct pci_dev *p = NULL;

	while ((p = pci_get_class(PCI_CLASS_STORAGE_SATA_AHCI, p)))
		if (eu_add(pci_dev_get(p), U32_MAX))
			pci_dev_put(p);
}

/* "DDDD:BB:DD.F/port[,...]" */
static void eu_parse_ports(void)
{
	char *s, *buf, *tok;

	if (!ports || !*ports)
		return;
	buf = kstrdup(ports, GFP_KERNEL);
	if (!buf)
		return;
	s = buf;
	while ((tok = strsep(&s, ","))) {
		unsigned int dom, bus, slot, fn, port;
		struct pci_dev *p;

		if (sscanf(tok, "%x:%x:%x.%x/%u", &dom, &bus, &slot, &fn, &port) != 5 ||
		    port >= EU_MAX_PORTS || bus > 0xff || slot > 0x1f || fn > 7) {
			pr_warn(DRV_NAME ": ports: cannot parse \"%s\"\n", tok);
			continue;
		}
		p = pci_get_domain_bus_and_slot(dom, bus, PCI_DEVFN(slot, fn));
		if (!p) {
			pr_warn(DRV_NAME ": ports: no PCI device %s\n", tok);
			continue;
		}
		if (eu_add(p, BIT(port)))
			pr_warn(DRV_NAME ": ports: too many controllers\n");
	}
	kfree(buf);
}

static int __init eu_init(void)
{
	int i, ret;

	if (dmi_check_system(eu_dmi_ds2415p))
		eu_find_ds2415p();
	else if (dmi_check_system(eu_dmi_synology))
		eu_find_synology();
	eu_parse_ports();
	if (!eu_ncands)
		return -ENODEV;

	ret = platform_driver_register(&eu_driver);
	if (ret)
		goto err_put;
	for (i = 0; i < eu_ncands; i++) {
		struct platform_device *pd;

		pd = platform_device_alloc(DRV_NAME, i);
		if (!pd) {
			ret = -ENOMEM;
			goto err_dev;
		}
		ret = platform_device_add(pd);
		if (ret) {
			platform_device_put(pd);
			goto err_dev;
		}
		eu_cands[i].plat = pd;
		/* probe once ahci has bound the controller, again after a rebind */
		if (!device_link_add(&pd->dev, &eu_cands[i].pdev->dev,
				     DL_FLAG_AUTOPROBE_CONSUMER)) {
			ret = -EINVAL;
			goto err_dev;
		}
	}
	return 0;

err_dev:
	for (i = 0; i < eu_ncands; i++) {
		if (!eu_cands[i].plat)
			continue;
		platform_device_unregister(eu_cands[i].plat);
		eu_cands[i].plat = NULL;
	}
	platform_driver_unregister(&eu_driver);
err_put:
	eu_put_cands();
	return ret;
}

static void __exit eu_exit(void)
{
	int i;

	for (i = 0; i < eu_ncands; i++)
		platform_device_unregister(eu_cands[i].plat);
	platform_driver_unregister(&eu_driver);
	eu_put_cands();
	ida_destroy(&eu_ida);
}

module_init(eu_init);
module_exit(eu_exit);

MODULE_DESCRIPTION("Synology expansion units with Marvell 88SM9705 port multipliers");
MODULE_LICENSE("GPL");
