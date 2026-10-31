/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Marvell 88SM9705 GPIO words of Synology expansion units, the write
 * policy of synology_eunit_9705.c and the unit table. Only integer types,
 * a const table and inline functions, no libata: a userspace test or
 * another driver (e.g. an out-of-tree module) can include this file as is
 * and share the exact same data. Outside the kernel, define u8, u16, u32
 * and bool before including it.
 *
 * Register word R of 0x3a0 (output data) and 0x3b0 (input), 20 bits:
 *   R[4:0]   data bits d[4:0]
 *   R[10:5]  not used here (always 0 in what the driver writes)
 *   R[13:11] data bits d[7:5]
 *   R[14]    write strobe, active low
 *   R[15]    read strobe, active low
 *   R[16]    not used here (always 0)
 *   R[18:17] page bits 1:0, R[19] page bit 2
 * The encoding is the same for every 88SM9705 unit. The output enable is
 * 0xfffff before a write and 0xfc7c0 before a read; a read is done with the
 * read strobe pulled low, a write with a low pulse of the write strobe.
 * Register numbers 0x3a0 (output), 0x3a4 (output
 * enable) and 0x3b0 (input) and the page meanings below were measured on a
 * DX1215 (four PMPs behind an 88SE9235 of a DS2415+): idle 0x3a0 0x6c000
 * (page 3, both strobes high) on all four PMPs.
 */
#ifndef SYNOLOGY_EUNIT_9705_H
#define SYNOLOGY_EUNIT_9705_H

#ifdef __KERNEL__
#include <linux/types.h>
#endif

#define EU_REG_OUT		0x3a0
#define EU_REG_EN		0x3a4
#define EU_REG_IN		0x3b0
#define EU_REG_POL		0x3ac	/* GPIO input polarity */
#define EU_POL_ALL		0xfffffU
#define EU_EN_ALL_OUT		0xfffffU	/* before every 0x3a0 write */
#define EU_EN_READ		0xfc7c0U	/* before reading 0x3b0 */
/*
 * Power-on state of a PMP whose GPIO block nobody has set up yet (measured on
 * the three slave PMPs of a DX1215 after a power cycle: 0x3a0 = 0, 0x3a4 =
 * 0x107c0). The driver sets such a PMP up (output enable 0xfffff) the first
 * time it meets this state, ending idle: both strobes high,
 * page 3 selected (0x6c000, measured on every PMP after set-up).
 */
#define EU_EN_POWERON		0x107c0U
/*
 * Input polarity (0x3ac): set to 0xfffff when a PMP is set up. Left at its
 * power-on value, every input bit reads inverted (measured: slave page 0 0xcc instead of 0x33).
 */
#define EU_R_IDLE		(EU_R_WCTL | EU_R_RCTL | EU_R_PAGE(3))

#define EU_R_WCTL		(1U << 14)	/* write strobe, active low */
#define EU_R_RCTL		(1U << 15)	/* read strobe, active low */
#define EU_R_PAGE(p)		((((p) & 3U) << 17) | ((((p) >> 2) & 1U) << 19))
#define EU_R_ALLOWED		(EU_R_WCTL | EU_R_RCTL | EU_R_PAGE(7))
/* a page select word: write strobe high, nothing but strobe and page bits */
#define EU_R_SELECT_OK(r)	(((r) & (EU_R_WCTL | ~EU_R_ALLOWED)) == EU_R_WCTL)
#define EU_R_SEL(p, rlow)	(EU_R_WCTL | ((rlow) ? 0 : EU_R_RCTL) | EU_R_PAGE(p))
/* data byte <-> its R bits: d[4:0] = R[4:0], d[7:5] = R[13:11] */
#define EU_DEC(r)		(((r) & 0x1fU) | (((r) >> 6) & 0xe0U))
#define EU_ENC(d)		(((d) & 0x1fU) | (((d) << 6) & 0x3800U))
/* a data word of page p: read strobe high, write strobe high or low */
#define EU_R_DATA(p, d, wlow)	(EU_R_RCTL | ((wlow) ? 0 : EU_R_WCTL) | \
				 EU_R_PAGE(p) | EU_ENC(d))

#define EU_PAGES		5
/* page 0: bits 7:5 EMID (0 = unit master), bits 4:0 unit ID */
#define EU_P0_ID(b)		((b) & 0x1fU)
#define EU_P0_EMID(b)		((b) >> 5)
/* page 1, per PMP: fault LED bits (per unit row), bit 6 all disk LEDs off */
#define EU_P1_BUZZER		0x20U	/* pulse line: never set */
#define EU_P1_LED_MASK		0x40U	/* 1 = all disk LEDs off */
#define EU_P1_NOT_LEDS		0xe0U	/* never a fault LED bit of a row */
/* page 2 on the master: bit n = fan n + 1 alarm */
/* page 4 on the master: bits 1:0 fan level, bit 4 a power-switch pulse */
#define EU_P4_FAN		0x03U
#define EU_P4_POWER_PULSE	0x10U	/* pulse line: never set */

enum { EU_FAN_FULL, EU_FAN_MIDDLE, EU_FAN_LOW, EU_FAN_STOP };

/*
 * State of one register cycle that the guard decides on. Filled only by
 * the cycle itself: the pages read in this EH round, and the page write
 * that is running (armed) with its byte as read (cur) and as written (new).
 */
struct eu_guard {
	bool writable;		/* the PMP belongs to a unit in the table */
	u8 id;			/* that row's unit ID */
	u8 p1_mask;		/* page 1 bits the row may change */
	unsigned int valid;	/* bit n: page[n] read in this round */
	u8 page[EU_PAGES];
	int armed;		/* page of the running write: 1, 4, -1 none */
	u8 cur, new;
};

/*
 * A data word for the page being written: the armed byte and nothing else;
 * page 0 of this round shows the row's unit ID; page 1 changes only the
 * row's bits and never has the buzzer bit; page 4 only on the master (EMID
 * 0), changes only bits 1:0, never the stop code, never the pulse bit.
 */
static inline bool eu_data_ok(const struct eu_guard *g, u32 val)
{
	unsigned int d = EU_DEC(val);
	int p = g->armed;

	if (!g->writable || (p != 1 && p != 4))
		return false;
	if (d != g->new || (val != EU_R_DATA(p, d, 0) && val != EU_R_DATA(p, d, 1)))
		return false;
	if (!(g->valid & 1U) || EU_P0_ID(g->page[0]) != g->id)
		return false;
	if (p == 4)
		return EU_P0_EMID(g->page[0]) == 0 &&
		       !((d | g->cur) & EU_P4_POWER_PULSE) &&
		       !((d ^ g->cur) & ~EU_P4_FAN) &&
		       (d & EU_P4_FAN) != EU_FAN_STOP;
	return !(g->p1_mask & EU_P1_BUZZER) &&
	       !((d | g->cur) & EU_P1_BUZZER) &&
	       !((d ^ g->cur) & ~g->p1_mask);
}

/* The write policy: true if (reg, val) may be sent to the PMP. */
static inline bool eu_write_ok(const struct eu_guard *g, u16 reg, u32 val)
{
	if (reg == EU_REG_EN)
		return val == EU_EN_ALL_OUT || val == EU_EN_READ;
	if (reg == EU_REG_POL)
		return val == EU_POL_ALL;
	if (reg == EU_REG_OUT)
		return EU_R_SELECT_OK(val) || eu_data_ok(g, val);
	return false;
}

/* new page 4 byte for fan level @code: bits 1:0 replaced, pulse bit 0 */
static inline u8 eu_p4_new(u8 cur, unsigned int code)
{
	return ((cur & ~EU_P4_FAN) | (code & EU_P4_FAN)) & ~EU_P4_POWER_PULSE;
}

/* new page 1 byte: the bits of @mask allowed by @p1_mask taken from @v */
static inline u8 eu_p1_new(u8 cur, u8 p1_mask, u8 mask, u8 v)
{
	u8 m = mask & p1_mask & ~EU_P1_BUZZER;

	return ((cur & ~m) | (v & m)) & ~EU_P1_BUZZER;
}

/* ---- unit table ---- */

#define EU_MAX_LOCAL		5	/* bays per PMP */
#define EU_MAX_FANS		4

struct eu_type {
	u8 id;
	const char *name;
	u8 bays, pmps, fans;
	u8 led_bit[EU_MAX_LOCAL];	/* page 1 bit of local bay 0.. */
	bool confirmed;
};

/*
 * One row per unit ID. Bays, PMPs and fans: Synology's published
 * specifications, three bays per PMP on the multi-PMP units. The
 * DX1222 LED bits are in reverse order. Rows other than the DX1215
 * are not confirmed on hardware yet.
 */
static const struct eu_type eu_types[] = {
	{ .id = 0x11, .name = "RX413", .bays = 4, .pmps = 1, .fans = 3,
	  .led_bit = { 0, 1, 2, 3 } },
	{ .id = 0x12, .name = "RX1214", .bays = 12, .pmps = 4, .fans = 4,
	  .led_bit = { 0, 1, 2 } },
	{ .id = 0x13, .name = "DX1215", .bays = 12, .pmps = 4, .fans = 2,
	  .led_bit = { 0, 1, 2 }, .confirmed = true },
	{ .id = 0x14, .name = "RX1217", .bays = 12, .pmps = 4, .fans = 3,
	  .led_bit = { 0, 1, 2 } },
	{ .id = 0x15, .name = "DX517", .bays = 5, .pmps = 1, .fans = 2,
	  .led_bit = { 0, 1, 2, 3, 4 } },
	{ .id = 0x16, .name = "RX418", .bays = 4, .pmps = 1, .fans = 3,
	  .led_bit = { 0, 1, 2, 3 } },
	{ .id = 0x17, .name = "DX1222", .bays = 12, .pmps = 4, .fans = 2,
	  .led_bit = { 3, 2, 1 } },
	{ .id = 0x1c, .name = "DX1215II", .bays = 12, .pmps = 4, .fans = 2,
	  .led_bit = { 0, 1, 2 } },
};

#define EU_NTYPES	(sizeof(eu_types) / sizeof(eu_types[0]))

static inline const struct eu_type *eu_type_lookup(u8 id)
{
	unsigned int i;

	for (i = 0; i < EU_NTYPES; i++)
		if (eu_types[i].id == id)
			return &eu_types[i];
	return NULL;
}

/* page 1 bits a row may change: its fault LED bits and the LED mask bit */
static inline u8 eu_type_p1_mask(const struct eu_type *t)
{
	unsigned int i;
	u8 m = EU_P1_LED_MASK;

	for (i = 0; i < (unsigned int)(t->bays / t->pmps); i++)
		m |= 1U << t->led_bit[i];
	return m;
}

#endif /* SYNOLOGY_EUNIT_9705_H */
