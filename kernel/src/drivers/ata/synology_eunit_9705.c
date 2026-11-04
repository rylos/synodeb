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
 * DX1215 row is confirmed on hardware. The other rows are read-only (fan
 * alarms, fan level, sysfs) unless unconfirmed_writes=1, and the log says
 * which row and whether it is confirmed. A PMP whose unit ID is not in the
 * table gets read-only sysfs files and nothing else.
 *
 * Nothing but reads goes to a PMP before page 0 has shown a unit ID of
 * the table: on a PMP found set up only the read protocol (page select
 * words, the two output-enable values), and a PMP found in the power-on
 * state is set up only when a master of a table unit on the same
 * controller is already identified (or with setup_unidentified=1). The
 * GPIO state found at the first look is logged once per PMP.
 *
 * Binding: the driver looks at AHCI ports of supported Synology hosts only
 * (DMI vendor "Synology Inc."; the DS2415+, whose BIOS has placeholder DMI
 * strings, by DMI or, when the kernel has no DMI, by the ACPI FADT OEM IDs,
 * and in both cases by PCI topology), plus the ports named with the "ports"
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

#include <linux/acpi.h>
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
#define EU_LED_GAP		HZ	/* at most one LED round per unit and second */
#define EU_P0_TRIES		5	/* page 0 tries, 30 s apart, then 60, 120, 240 s */
#define EU_MASTER_PAGES		(BIT(0) | BIT(2) | BIT(4))
#define EU_ALL_PAGES		(BIT(EU_PAGES) - 1)

static const u8 eu_level_pwm[4] = { 255, 128, 64, 0 };

static bool unconfirmed_writes;
module_param(unconfirmed_writes, bool, 0444);
MODULE_PARM_DESC(unconfirmed_writes, "Fan level and LED writes on unit rows not yet confirmed on hardware");

static bool disk_leds = true;
module_param(disk_leds, bool, 0444);
MODULE_PARM_DESC(disk_leds, "Turn the disk LEDs of a unit on when it is found (page 1 bit 6)");

static unsigned int refresh_s = 10;
module_param(refresh_s, uint, 0644);
MODULE_PARM_DESC(refresh_s, "Max age in s of the fan values before a read asks the unit again");

static bool setup_unidentified;
module_param(setup_unidentified, bool, 0444);
MODULE_PARM_DESC(setup_unidentified, "Set up 88SM9705 PMPs found in the power-on state even without an identified unit on the controller");

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
	bool p1_valid;			/* page 1 as last read */
	u8 p1;
	bool logged;			/* GPIO state at first look logged */
	bool parked;			/* power-on state, left as it is */
	bool given_up;			/* page 0 failed EU_P0_TRIES times */
	unsigned int fails;
	unsigned long next;		/* no page 0 try before this */
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
	struct delayed_work led_work;	/* LED writes, coalesced */
	unsigned long led_stamp;	/* last LED round */
	u32 led_mask, led_val;		/* bit = bay: LED to set, its value */
	unsigned int led_fails;		/* failed LED rounds in a row */
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
	bool suspended;			/* no LED round until resume */
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
	bool setup;			/* may set up a PMP in the power-on state */
	u32 out, en;			/* 0x3a0/0x3a4 to leave behind */
	bool found;			/* out0, en0, pol0 read */
	u32 out0, en0, pol0;		/* 0x3a0/0x3a4/0x3ac as found */
	u32 pol;			/* 0x3ac now: how 0x3b0 is decoded */
};

static bool eu_writable(const struct eu_unit *u)
{
	return u && eu_type_writable(u->type, unconfirmed_writes);
}

/*
 * @u: the unit the PMP belongs to, NULL while it is not identified. A
 * unit's request may set the PMP up again (after the unit was power
 * cycled); fan and LED writes only for a writable row.
 */
static void eu_req_init(struct eu_req *r, const struct eu_unit *u,
			unsigned int pages)
{
	memset(r, 0, sizeof(*r));
	r->pages = pages;
	r->wpage = -1;
	r->g.armed = -1;
	if (u && u->type) {
		r->setup = true;
		r->g.writable = eu_writable(u);
		r->g.id = u->type->id;
		r->g.p1_mask = eu_type_p1_mask(u->type);
	}
}

/* The only PMP write of the driver. */
static unsigned int eu_write(struct ata_port *ap, struct eu_req *r, u16 reg,
			     u32 val)
{
	unsigned int err;

	if (!eu_write_ok(&r->g, reg, val)) {
		WARN_ONCE(1, DRV_NAME ": refused PMP write reg 0x%x val 0x%x\n",
			  reg, val);
		return AC_ERR_INVALID;
	}
	err = sata_pmp_vendor_write(ap, reg, val);
	if (!err && reg == EU_REG_EN)
		r->g.en_now = val;
	return err;
}

/*
 * output enable all-out, then 0x3a0; 5 ms for the unit, with the EH of the
 * controller's other ports free to run meanwhile
 */
static unsigned int eu_out(struct ata_port *ap, struct eu_req *r, u32 val)
{
	unsigned int err = eu_write(ap, r, EU_REG_EN, EU_EN_ALL_OUT);

	if (!err)
		err = eu_write(ap, r, EU_REG_OUT, val);
	ata_msleep(ap, 5);
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
		r->g.page[pg] = eu_dec_in(raw, r->pol);
		r->g.valid |= BIT(pg);
	}
	return err;
}

/*
 * 0x3a0 and 0x3a4 to the state to leave behind, read back. Back to the
 * power-on state: strobes high first, then 0x3a4, then 0x3a0 0.
 */
static unsigned int eu_restore(struct ata_port *ap, struct eu_req *r)
{
	unsigned int err;
	u32 out, en;

	if (r->en == EU_EN_POWERON) {
		err = eu_out(ap, r, EU_R_IDLE);
		if (!err)
			err = eu_write(ap, r, EU_REG_EN, EU_EN_POWERON);
		if (!err)
			err = eu_write(ap, r, EU_REG_OUT, 0);
	} else {
		err = eu_out(ap, r, r->out);
		if (!err)
			err = eu_write(ap, r, EU_REG_EN, r->en);
	}
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

/* from the EH callback; outside EH use eu_is_9705_locked() */
static bool eu_is_9705(struct ata_port *ap)
{
	u32 *gscr = ap->link.device->gscr;

	return sata_pmp_attached(ap) &&
	       sata_pmp_gscr_vendor(gscr) == PCI_VENDOR_ID_MARVELL_EXT &&
	       sata_pmp_gscr_devid(gscr) == 0x9705;
}

static bool eu_is_9705_locked(struct ata_port *ap)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(ap->lock, flags);
	ret = eu_is_9705(ap);
	spin_unlock_irqrestore(ap->lock, flags);
	return ret;
}

/*
 * Power-on state to the set-up idle state: 0x3a0 gets the idle word while
 * 0x3a4 still has only the power-on outputs, then 0x3a4 all-out, so the
 * strobes come up high. If 0x3a0 did not keep the idle word, it is written
 * again.
 */
static unsigned int eu_setup(struct ata_port *ap, struct eu_req *r)
{
	unsigned int err;
	u32 out;

	err = eu_write(ap, r, EU_REG_OUT, EU_R_IDLE);
	if (!err)
		err = eu_write(ap, r, EU_REG_EN, EU_EN_ALL_OUT);
	if (!err)
		err = sata_pmp_vendor_read(ap, EU_REG_OUT, &out);
	if (!err && out != EU_R_IDLE)
		err = eu_out(ap, r, EU_R_IDLE);
	return err;
}

/*
 * Reads 0x3a0, 0x3a4, 0x3ac first and writes nothing before page 0 has
 * identified the unit, apart from the read protocol itself (page select
 * words, the two output-enable values) on a PMP found set up, and the
 * set-up of a PMP found in the power-on state when the request allows it.
 * Page 0 without a table unit ID (or not the request's row): no other
 * page, no write, the PMP is left as found, return -ENODEV with page 0 in
 * r->g. A PMP in the power-on state that may not be set up: -EPERM; an
 * input polarity other than 0xfffff or 0: -ENXIO; nothing written.
 */
static int eu_cycle_fn(struct ata_port *ap, void *data)
{
	struct eu_req *r = data;
	unsigned int pages = r->pages, err;
	int pg, rc = 0;
	u32 pol;

	r->g.armed = -1;
	r->g.valid = 0;
	r->g.poweron = false;
	r->g.setup_ok = r->setup;
	r->found = false;
	if (!eu_is_9705(ap))
		return -ENODEV;
	err = sata_pmp_vendor_read(ap, EU_REG_OUT, &r->out0);
	if (!err)
		err = sata_pmp_vendor_read(ap, EU_REG_EN, &r->en0);
	if (!err)
		err = sata_pmp_vendor_read(ap, EU_REG_POL, &r->pol0);
	if (err)
		return -EIO;
	r->found = true;
	r->pol = r->pol0;
	r->out = r->out0;
	r->en = r->en0;
	r->g.en_now = r->en0;
	if (r->out0 == 0 && r->en0 == EU_EN_POWERON) {
		r->g.poweron = true;
		if (!r->setup)
			return -EPERM;
	}
	if (!eu_pol_known(r->pol0))
		return -ENXIO;
	if (r->g.poweron) {
		err = eu_setup(ap, r);
		if (err)
			goto restore;
	} else if (!EU_R_SELECT_OK(r->out0) ||
		   (r->en0 != EU_EN_ALL_OUT && r->en0 != EU_EN_READ)) {
		/* not a state we could put back: leave the PMP alone */
		return -EIO;
	}

	/* identification */
	err = eu_read_page(ap, r, 0);
	if (err)
		goto restore;
	if (!eu_id_ok(&r->g)) {
		rc = -ENODEV;
		goto restore;
	}
	/* identified: input polarity 0xfffff; a set-up PMP stays set up */
	if (r->pol != EU_POL_ALL) {
		err = eu_write(ap, r, EU_REG_POL, EU_POL_ALL);
		if (!err)
			err = sata_pmp_vendor_read(ap, EU_REG_POL, &pol);
		if (!err && pol != EU_POL_ALL)
			err = AC_ERR_OTHER;
		if (err)
			goto restore;
		r->pol = pol;
	}
	if (r->g.poweron) {
		r->out = EU_R_IDLE;
		r->en = EU_EN_ALL_OUT;
	}

	/* a write needs page 0 (unit ID, EMID) and the page itself from this round */
	if (r->wpage >= 0)
		pages |= BIT(r->wpage);
	for (pg = 1; pg < EU_PAGES && !err; pg++)
		if (pages & BIT(pg))
			err = eu_read_page(ap, r, pg);
	if (!err && r->wpage >= 0)
		rc = eu_write_page(ap, r);
restore:
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

/* caller holds lock: keep page 1 of a port as read in @r */
static void eu_note_p1(struct eu_port *p, const struct eu_req *r)
{
	if (r->g.valid & BIT(1)) {
		p->p1 = r->g.page[1];
		p->p1_valid = true;
	}
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

	if (!eu_writable(u))
		return -EPERM;
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
		return eu_writable(u) ? 0644 : 0444;
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

/*
 * LED writes: each is an EH round that pauses I/O on the unit's disks, so
 * they are collected and written by eu_led_work_fn(), at most once per
 * unit and EU_LED_GAP, only the bits that differ from page 1 as last read.
 */
static int eu_led_set(struct led_classdev *cdev, enum led_brightness value)
{
	struct eu_led *led = container_of(cdev, struct eu_led, cdev);
	struct eu_unit *u = led->unit;
	unsigned long due;

	mutex_lock(&u->ctrl->lock);
	if (u->gone) {
		mutex_unlock(&u->ctrl->lock);
		return -ENODEV;
	}
	u->led_mask |= BIT(led->bay);
	if (value)
		u->led_val |= BIT(led->bay);
	else
		u->led_val &= ~BIT(led->bay);
	/* while suspended the bits wait in led_mask; resume queues them */
	if (!u->ctrl->suspended) {
		due = u->led_stamp + EU_LED_GAP;
		schedule_delayed_work(&u->led_work,
				      time_after(jiffies, due) ? 0 : due - jiffies);
	}
	mutex_unlock(&u->ctrl->lock);
	return 0;
}

static void eu_led_work_fn(struct work_struct *work)
{
	struct eu_unit *u = container_of(to_delayed_work(work), struct eu_unit,
					 led_work);
	struct eu_ctrl *c = u->ctrl;
	unsigned int per = u->type->bays / u->type->pmps, i;
	bool again = false;
	int e, ret;

	mutex_lock(&c->lock);
	if (c->suspended) {
		mutex_unlock(&c->lock);
		return;
	}
	for (e = 0; e < u->npmps && !u->gone; e++) {
		struct eu_port *p;
		struct eu_req r;
		u8 m = 0, v = 0, d;
		u32 tried = 0;

		for (i = 0; i < per; i++) {
			unsigned int bay = e * per + i;

			if (!(u->led_mask & BIT(bay)))
				continue;
			u->led_mask &= ~BIT(bay);
			tried |= BIT(bay);
			m |= BIT(u->type->led_bit[i]);
			if (u->led_val & BIT(bay))
				v |= BIT(u->type->led_bit[i]);
		}
		if (!m || u->pmp[e] < 0)
			continue;
		p = &c->port[u->pmp[e]];
		d = p->p1_valid ?
		    eu_p1_diff(p->p1, eu_type_p1_mask(u->type), m, v) : m;
		if (!d)
			continue;
		eu_req_init(&r, u, 0);
		r.wpage = 1;
		r.wmask = d;
		r.wval = v;
		ret = eu_cycle(p->ap, &r);
		eu_note_p1(p, &r);
		u->led_stamp = jiffies;
		if (!ret)
			continue;
		if (eu_led_retry(ret, u->led_fails)) {
			/* pending again; a newer request keeps its value */
			u->led_mask = eu_led_requeue(u->led_mask, tried);
			again = true;
		} else if (ret == -EIO || ret == -EAGAIN) {
			dev_warn_ratelimited(c->dev,
					     "ata%u: LEDs not set after %u tries: %d\n",
					     p->ap->print_id, u->led_fails + 1, ret);
		} else {
			dev_warn_ratelimited(c->dev, "ata%u: LEDs: %d\n",
					     p->ap->print_id, ret);
		}
	}
	if (again && !u->gone) {
		u->led_fails++;
		schedule_delayed_work(&u->led_work, EU_LED_GAP);
	} else {
		u->led_fails = 0;
	}
	mutex_unlock(&c->lock);
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
		/* read-only: the guard allows no data word */
		eu_req_init(&r, u, EU_ALL_PAGES);
		r.g.writable = false;
		ret = eu_cycle(ap, &r);
		n += sysfs_emit_at(buf, n, "ata%u", ap->print_id);
		if (ret == -ENODEV && (r.g.valid & BIT(0))) {
			/* no table unit ID: page 0 only */
			n += sysfs_emit_at(buf, n, " %02x\n", r.g.page[0]);
			continue;
		}
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

	if (!disk_leds || !eu_writable(u) || !ap)
		return;
	eu_req_init(&r, u, 0);
	r.wpage = 1;
	r.wmask = EU_P1_LED_MASK;
	r.wval = 0;
	ret = eu_cycle(ap, &r);
	eu_note_p1(&u->ctrl->port[u->pmp[emid]], &r);
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
	cancel_delayed_work_sync(&u->led_work);
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
	INIT_DELAYED_WORK(&u->led_work, eu_led_work_fn);
	u->led_stamp = jiffies - EU_LED_GAP;
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

	if (t && t->confirmed)
		dev_info(c->dev, "ata%u: unit ID 0x%02x, using row %s (confirmed)\n",
			 p->ap->print_id, EU_P0_ID(p->p0), t->name);
	else if (t)
		dev_info(c->dev, "ata%u: unit ID 0x%02x, using row %s (unconfirmed, please report): fan level and LEDs %s\n",
			 p->ap->print_id, EU_P0_ID(p->p0), t->name,
			 unconfirmed_writes ? "writable (unconfirmed_writes=1)" :
			 "read-only, unconfirmed_writes=1 makes them writable");
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
	if (!eu_writable(u))
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

/*
 * caller holds lock: the unit ID of a master (EMID 0) of a table unit on
 * this controller, found by page 0, or 0. Its slaves may be set up from the
 * power-on state.
 */
static u8 eu_master_id(struct eu_ctrl *c)
{
	int i;

	for (i = 0; i < EU_MAX_PORTS; i++) {
		struct eu_port *p = &c->port[i];

		if (p->ap && p->p0_valid && !EU_P0_EMID(p->p0) &&
		    eu_type_lookup(EU_P0_ID(p->p0)))
			return EU_P0_ID(p->p0);
	}
	return 0;
}

/* caller holds lock: page 0 of a PMP not identified yet */
static void eu_identify(struct eu_ctrl *c, struct eu_port *p)
{
	u8 mid = eu_master_id(c);
	struct eu_req r;
	int ret;

	if (p->given_up || (p->fails && time_before(jiffies, p->next)))
		return;
	/* power-on state: nothing changed since, no command needed */
	if (p->parked && !mid && !setup_unidentified)
		return;
	eu_req_init(&r, NULL, BIT(0));
	r.setup = mid || setup_unidentified;
	r.g.id = setup_unidentified ? 0 : mid;
	ret = eu_cycle(p->ap, &r);
	if (r.found && !p->logged) {
		p->logged = true;
		dev_info(c->dev, "ata%u: GPIO at first look: 0x3a0 0x%05x 0x3a4 0x%05x 0x3ac 0x%05x\n",
			 p->ap->print_id, r.out0, r.en0, r.pol0);
	}
	if ((!ret || ret == -ENODEV) && (r.g.valid & BIT(0))) {
		/* identified, or a unit ID that is not ours: no more tries */
		p->p0 = r.g.page[0];
		p->p0_valid = true;
		p->parked = false;
		p->fails = 0;
		return;
	}
	if (ret == -ENXIO) {
		p->given_up = true;
		dev_warn(c->dev, "ata%u: input polarity 0x%05x not known, left as it is\n",
			 p->ap->print_id, r.pol0);
		return;
	}
	if (ret == -EPERM) {
		if (!p->parked)
			dev_info(c->dev, "ata%u: GPIO in the power-on state, left as it is (no identified unit on this controller; setup_unidentified=1 sets it up)\n",
				 p->ap->print_id);
		p->parked = true;
		return;
	}
	if (++p->fails >= EU_P0_TRIES) {
		p->given_up = true;
		dev_warn(c->dev, "ata%u: page 0: %d, giving up after %u tries\n",
			 p->ap->print_id, ret, p->fails);
		return;
	}
	p->next = jiffies + (EU_SCAN_S << (p->fails - 1)) * HZ;
}

/* caller holds lock: forget everything about a port whose PMP went away */
static void eu_port_reset(struct eu_port *p)
{
	struct ata_port *ap = p->ap;

	memset(p, 0, sizeof(*p));
	p->ap = ap;
}

/* caller holds lock: drop what went away, read page 0 of new PMPs, group */
static void eu_scan(struct eu_ctrl *c, struct list_head *dead)
{
	struct eu_unit *u, *tmp;
	int i, e;

	for (i = 0; i < EU_MAX_PORTS; i++) {
		struct eu_port *p = &c->port[i];

		if (!p->ap)
			continue;
		if (!eu_is_9705_locked(p->ap)) {
			u = p->unit;
			if (u && u->pmp[0] == i) {
				u->gone = true;
			} else if (u) {
				for (e = 0; e < u->npmps; e++)
					if (u->pmp[e] == i)
						u->pmp[e] = -1;
			}
			eu_port_reset(p);
			continue;
		}
		if (!p->unit && !p->p0_valid)
			eu_identify(c, p);
	}

	list_for_each_entry_safe(u, tmp, &c->units, node) {
		if (!u->gone)
			continue;
		dev_info(c->dev, "%s (syno-eunit%d) gone\n", u->hwmon_name, u->index);
		for (e = 0; e < u->npmps; e++) {
			if (u->pmp[e] < 0)
				continue;
			eu_port_reset(&c->port[u->pmp[e]]);
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

/*
 * No scan or LED round while the system suspends; the unit list stays.
 * LED requests made meanwhile (a trigger) are kept and written on resume.
 */
static int eu_suspend(struct device *dev)
{
	struct eu_ctrl *c = dev_get_drvdata(dev);
	struct eu_unit *u;

	mutex_lock(&c->lock);
	c->suspended = true;
	mutex_unlock(&c->lock);
	cancel_delayed_work_sync(&c->work);
	/*
	 * An LED round in progress holds the lock, so none runs once it is
	 * taken here; one that starts later sees suspended and returns. The
	 * requested bits stay in led_mask.
	 */
	mutex_lock(&c->lock);
	list_for_each_entry(u, &c->units, node)
		cancel_delayed_work(&u->led_work);
	mutex_unlock(&c->lock);
	return 0;
}

static int eu_resume(struct device *dev)
{
	struct eu_ctrl *c = dev_get_drvdata(dev);
	struct eu_unit *u;
	int i;

	mutex_lock(&c->lock);
	c->suspended = false;
	list_for_each_entry(u, &c->units, node) {
		u->mvalid = false;
		if (u->led_mask)
			schedule_delayed_work(&u->led_work, 0);
	}
	for (i = 0; i < EU_MAX_PORTS; i++)
		c->port[i].p1_valid = false;
	mutex_unlock(&c->lock);
	schedule_delayed_work(&c->work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(eu_pm, eu_suspend, eu_resume);

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

/*
 * DS2415+ without DMI: a kernel started by kexec without EFI tables (the
 * DS2415+ BIOS publishes SMBIOS only through EFI, there is no legacy entry
 * point) sees no DMI, but ACPI is there. Measured on a DS2415+ (BIOS M.211):
 * every table has OEM ID "INSYDE" and OEM table ID "MOHOPEAK". Those are
 * Insyde's Avoton reference-board IDs, so eu_find_ds2415p() still checks
 * the PCI topology. The module alias below is the Avoton PCU, so the
 * module also loads on such a kernel.
 */
static bool eu_acpi_ds2415p(void)
{
#ifdef CONFIG_ACPI
	struct acpi_table_header *h;
	bool ok;

	if (acpi_disabled ||
	    ACPI_FAILURE(acpi_get_table(ACPI_SIG_FADT, 0, &h)))
		return false;
	ok = !memcmp(h->oem_id, "INSYDE", ACPI_OEM_ID_SIZE) &&
	     !memcmp(h->oem_table_id, "MOHOPEAK", ACPI_OEM_TABLE_ID_SIZE);
	acpi_put_table(h);
	return ok;
#else
	return false;
#endif
}

static const struct pci_device_id eu_pci_avoton[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x1f38) },
	{ }
};
MODULE_DEVICE_TABLE(pci, eu_pci_avoton);

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

	if (dmi_check_system(eu_dmi_ds2415p) ||
	    (!dmi_get_system_info(DMI_SYS_VENDOR) && eu_acpi_ds2415p()))
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
