// SPDX-License-Identifier: GPL-2.0
/*
 * Userspace test of kernel/src/drivers/ata/synology_eunit_9705.h: the GPIO
 * word encoding, the unit table and the write guard of the
 * synology_eunit_9705 driver. Run by run-tests.sh, which also shows that it
 * fails on deliberately broken copies of the header.
 *
 * The expected values are computed here independently: the encoding from
 * a (page, strobes, data) value, page in bits 12:10, read strobe bit 9,
 * write strobe bit 8, data in bits 7:0, mapped bit by bit to the register
 * word; the guard from a reference model of the write policy.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#include EU_HEADER

static unsigned long fails, checks;

#define EXPECT(c, ...) do {						\
	checks++;							\
	if (!(c)) {							\
		if (++fails <= 20) {					\
			printf("FAIL: " __VA_ARGS__);			\
			printf("\n");					\
		}							\
	}								\
} while (0)

/* bit-by-bit mapping of a (page, strobes, data) value to the register word */
static u32 gpl_enc(u32 v)
{
	return (v & 0x1f) | ((v & 0x3e0) << 6) | ((v & 0x1c00) << 7);
}

/* value of GPIO_9705_PKG_INIT(page, data): both strobes high */
static u32 gpl_pkg(u32 page, u32 data)
{
	return (page << 10) | (0x3 << 8) | data;
}

/* ---- reference model of the write policy ---- */

static bool ref_data_ok(const struct eu_guard *g, u32 val)
{
	u32 w0, w1;
	u8 d = g->new;

	if (!g->writable)
		return false;			/* unknown unit: no data word ever */
	if (g->armed != 1 && g->armed != 4)
		return false;			/* no write running */
	w0 = gpl_enc(gpl_pkg(g->armed, d));
	w1 = gpl_enc(gpl_pkg(g->armed, d) & ~0x100u);	/* write strobe low */
	if (val != w0 && val != w1)
		return false;			/* only the armed byte, as encoded */
	if (!(g->valid & 1) || (g->page[0] & 0x1f) != g->id)
		return false;			/* page 0 of this round shows the row */
	if (g->armed == 4) {
		if (g->page[0] >> 5)
			return false;		/* master only */
		if ((d & 0x10) || (g->cur & 0x10))
			return false;		/* power pulse */
		if ((d ^ g->cur) & ~0x03)
			return false;		/* only the fan bits */
		return (d & 3) != 3;		/* never stop */
	}
	if ((d & 0x20) || (g->cur & 0x20) || (g->p1_mask & 0x20))
		return false;			/* buzzer pulse */
	return !((d ^ g->cur) & ~g->p1_mask);
}

/* page 0 of this round: a table unit ID (the 8 IDs), the request's if any */
static bool ref_id_ok(const struct eu_guard *g)
{
	static const u8 ids[] = { 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1c };
	unsigned int i;
	u8 id = g->page[0] & 0x1f;

	if (!(g->valid & 1))
		return false;
	for (i = 0; i < sizeof(ids); i++)
		if (ids[i] == id)
			return !g->id || id == g->id;
	return false;
}

static bool ref_write_ok(const struct eu_guard *g, u16 reg, u32 val)
{
	u32 sel_bits = (1u << 14) | (1u << 15) | (7u << 17);

	if (g->poweron && !g->setup_ok)
		return false;			/* power-on state: hands off */
	if (reg == 0x3a4) {
		if (val == 0x107c0)
			return g->poweron;	/* back to the power-on state */
		return val == 0xfffff || val == 0xfc7c0;
	}
	if (reg == 0x3ac)		/* GPI polarity 0xfffff, after identification */
		return val == 0xfffff && ref_id_ok(g);
	if (reg != 0x3a0)
		return false;
	if (g->en_now == 0x107c0)	/* strobe pins are inputs: idle or 0 */
		return g->poweron && (val == 0x6c000 || val == 0);
	/* page select: write strobe high, only strobes and page bits */
	if ((val & (1u << 14)) && !(val & ~sel_bits))
		return true;
	return ref_data_ok(g, val);
}

/* ---- tests ---- */

static void test_encoding(void)
{
	u32 p, d, r;

	for (p = 0; p < 8; p++) {
		EXPECT(EU_R_SEL(p, 0) == gpl_enc(gpl_pkg(p, 0)),
		       "SEL(%u,0) 0x%x", p, EU_R_SEL(p, 0));
		EXPECT(EU_R_SEL(p, 1) == gpl_enc(gpl_pkg(p, 0) & ~0x200u),
		       "SEL(%u,1) 0x%x", p, EU_R_SEL(p, 1));
		EXPECT(EU_R_SELECT_OK(EU_R_SEL(p, 0)) && EU_R_SELECT_OK(EU_R_SEL(p, 1)),
		       "SEL(%u) not a select word", p);
		for (d = 0; d < 256; d++) {
			EXPECT(EU_R_DATA(p, d, 0) == gpl_enc(gpl_pkg(p, d)),
			       "DATA(%u,0x%x,0)", p, d);
			EXPECT(EU_R_DATA(p, d, 1) == gpl_enc(gpl_pkg(p, d) & ~0x100u),
			       "DATA(%u,0x%x,1)", p, d);
			EXPECT(EU_DEC(EU_R_DATA(p, d, 0)) == d && EU_DEC(EU_R_DATA(p, d, 1)) == d,
			       "DEC(DATA(%u,0x%x))", p, d);
			EXPECT(!EU_R_SELECT_OK(EU_R_DATA(p, d, 1)),
			       "write-strobe word (%u,0x%x) passes as select", p, d);
		}
	}
	/* measured: idle 0x3a0 on all four DX1215 PMPs = page 3, strobes high */
	EXPECT(EU_R_SEL(3, 0) == 0x6c000, "SEL(3,0) 0x%x != measured 0x6c000", EU_R_SEL(3, 0));
	/* select words: exhaustive */
	for (r = 0; r < (1u << 20); r++) {
		bool want = (r & (1u << 14)) && !(r & ~((1u << 14) | (1u << 15) | (7u << 17)));

		if (EU_R_SELECT_OK(r) != want) {
			EXPECT(0, "SELECT_OK(0x%x)", r);
			break;
		}
	}
	EXPECT(EU_EN_ALL_OUT == 0xfffff && EU_EN_READ == 0xfc7c0, "output enable values");
	EXPECT(EU_REG_OUT == 0x3a0 && EU_REG_EN == 0x3a4 && EU_REG_IN == 0x3b0 &&
	       EU_REG_POL == 0x3ac, "registers");
}

static void test_table(void)
{
	/* the 88SM9705 unit IDs */
	static const u8 gpl_ids[] = { 0x11, 0x12, 0x14, 0x13, 0x15, 0x16, 0x17, 0x1c };
	/* id, bays, pmps, fans as specified for this driver */
	static const u8 want[][4] = {
		{ 0x13, 12, 4, 2 }, { 0x11, 4, 1, 3 }, { 0x12, 12, 4, 4 },
		{ 0x14, 12, 4, 3 }, { 0x15, 5, 1, 2 }, { 0x16, 4, 1, 3 },
		{ 0x17, 12, 4, 2 }, { 0x1c, 12, 4, 2 },
	};
	unsigned int i, j, n = 0;

	EXPECT(EU_NTYPES == 8, "%zu rows", (size_t)EU_NTYPES);
	for (i = 0; i < sizeof(gpl_ids); i++)
		EXPECT(eu_type_lookup(gpl_ids[i]), "id 0x%02x missing", gpl_ids[i]);
	for (i = 0; i < 32; i++)
		if (eu_type_lookup(i))
			n++;
	EXPECT(n == 8, "%u ids found", n);
	for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		const struct eu_type *t = eu_type_lookup(want[i][0]);

		if (!t) {
			EXPECT(0, "row 0x%02x", want[i][0]);
			continue;
		}
		EXPECT(t->bays == want[i][1] && t->pmps == want[i][2] && t->fans == want[i][3],
		       "%s: bays %u pmps %u fans %u", t->name, t->bays, t->pmps, t->fans);
		EXPECT(t->confirmed == (t->id == 0x13), "%s confirmed %d", t->name, t->confirmed);
	}
	for (i = 0; i < EU_NTYPES; i++) {
		const struct eu_type *t = &eu_types[i];
		unsigned int per = t->bays / t->pmps;
		u8 m = 0;

		EXPECT(t->pmps >= 1 && t->pmps <= 4 && t->bays % t->pmps == 0 &&
		       per <= EU_MAX_LOCAL && t->fans <= EU_MAX_FANS, "%s shape", t->name);
		for (j = 0; j < per; j++) {
			EXPECT(t->led_bit[j] < 5, "%s LED bit %u", t->name, t->led_bit[j]);
			EXPECT(!(m & (1u << t->led_bit[j])), "%s LED bit %u twice", t->name, t->led_bit[j]);
			m |= 1u << t->led_bit[j];
		}
		EXPECT(!(eu_type_p1_mask(t) & (EU_P1_BUZZER | 0x80)),
		       "%s page 1 mask 0x%02x has a pulse/not-ours bit", t->name, eu_type_p1_mask(t));
		EXPECT(eu_type_p1_mask(t) == (m | 0x40), "%s p1 mask", t->name);
	}
	EXPECT(eu_type_p1_mask(eu_type_lookup(0x13)) == 0x47, "DX1215 page 1 mask (measured 0x47)");
	EXPECT(eu_type_lookup(0x17)->led_bit[0] == 3 && eu_type_lookup(0x17)->led_bit[2] == 1,
	       "DX1222 LED order reversed");
}

static void guard_for(struct eu_guard *g, const struct eu_type *t, u8 p0, int armed,
		      u8 cur, u8 new)
{
	g->writable = t != NULL;
	g->id = t ? t->id : 0;
	g->poweron = false;
	g->setup_ok = false;
	g->en_now = EU_EN_ALL_OUT;
	g->p1_mask = t ? eu_type_p1_mask(t) : 0;
	g->valid = 0x1f;
	g->page[0] = p0;
	g->page[1] = cur;
	g->page[4] = cur;
	g->armed = armed;
	g->cur = cur;
	g->new = new;
}

static void test_guard(void)
{
	static const u8 curs[] = { 0x00, 0x02, 0x03, 0x10, 0x12, 0x20, 0x40, 0x47, 0x80, 0xff };
	static const int armeds[] = { -1, 1, 4 };
	unsigned int ti, ci, ai, pi, d, accepted = 0, stop = 0, pulse = 0;
	struct eu_guard g;
	u32 v;

	for (ti = 0; ti <= EU_NTYPES; ti++) {
		const struct eu_type *t = ti < EU_NTYPES ? &eu_types[ti] : NULL;
		u8 id = t ? t->id : 0x1d;
		const u8 p0s[] = { id, (u8)(id | 0x20), (u8)(id | 0x60), (u8)(id ^ 0x01) };

		for (ai = 0; ai < 3; ai++)
			for (ci = 0; ci < sizeof(curs); ci++)
				for (pi = 0; pi < 4; pi++)
					for (d = 0; d < 256; d++) {
						int p = armeds[ai] < 0 ? 4 : armeds[ai];
						u32 w[2] = { EU_R_DATA(p, d, 0), EU_R_DATA(p, d, 1) };
						int k;

						guard_for(&g, t, p0s[pi], armeds[ai], curs[ci], d);
						for (k = 0; k < 2; k++) {
							bool ok = eu_write_ok(&g, EU_REG_OUT, w[k]);

							EXPECT(ok == ref_write_ok(&g, EU_REG_OUT, w[k]),
							       "%s armed %d cur 0x%02x p0 0x%02x new 0x%02x word %d: %d",
							       t ? t->name : "unknown", armeds[ai], curs[ci],
							       p0s[pi], d, k, ok);
							accepted += ok;
							if (ok && p == 4 && (d & 3) == 3)
								stop++;
							if (ok && ((p == 4 && (d & 0x10)) || (p == 1 && (d & 0x20))))
								pulse++;
						}
						/* a data word for another byte than the armed one */
						EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(p, (d + 1) & 0xff, 1)),
						       "non-armed byte accepted");
					}
	}
	EXPECT(accepted > 0, "no data word accepted at all");
	EXPECT(stop == 0, "%u stop words accepted", stop);
	EXPECT(pulse == 0, "%u pulse words accepted", pulse);

	/* every 20-bit word, both registers, in a few armed states */
	for (ti = 0; ti < EU_NTYPES; ti++) {
		const struct eu_type *t = &eu_types[ti];

		for (ai = 1; ai < 3; ai++) {
			guard_for(&g, t, t->id, armeds[ai], 0x02, armeds[ai] == 4 ? 0x01 : 0x03);
			for (v = 0; v < (1u << 20); v++) {
				if (eu_write_ok(&g, EU_REG_OUT, v) != ref_write_ok(&g, EU_REG_OUT, v) ||
				    eu_write_ok(&g, EU_REG_EN, v) != ref_write_ok(&g, EU_REG_EN, v) ||
				    eu_write_ok(&g, EU_REG_POL, v) != ref_write_ok(&g, EU_REG_POL, v) ||
				    eu_write_ok(&g, EU_REG_IN, v) || eu_write_ok(&g, 0x3a8, v)) {
					EXPECT(0, "%s armed %d word 0x%05x", t->name, armeds[ai], v);
					break;
				}
			}
		}
	}

	/* concrete cases */
	guard_for(&g, eu_type_lookup(0x13), 0x13, 4, 0x02, 0x00);	/* DX1215 low -> full */
	EXPECT(eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(4, 0x00, 1)), "DX1215 full refused");
	g.new = 0x03;
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(4, 0x03, 1)), "DX1215 stop accepted");
	g.new = 0x12;
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(4, 0x12, 1)), "power pulse accepted");
	guard_for(&g, eu_type_lookup(0x13), 0x33, 4, 0x02, 0x00);	/* EMID 1 */
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(4, 0x00, 1)), "page 4 on a slave accepted");
	guard_for(&g, eu_type_lookup(0x13), 0x33, 1, 0x40, 0x41);	/* slave LED */
	EXPECT(eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(1, 0x41, 1)), "slave LED refused");
	g.new = 0x48;
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(1, 0x48, 1)), "DX1215 page 1 bit 3 accepted");
	g.new = 0x60;
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(1, 0x60, 1)), "buzzer accepted");
	guard_for(&g, eu_type_lookup(0x17), 0x17, 1, 0x40, 0x48);	/* DX1222 bit 3 */
	EXPECT(eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(1, 0x48, 1)), "DX1222 bay LED bit 3 refused");
	guard_for(&g, eu_type_lookup(0x13), 0x15, 4, 0x02, 0x00);	/* other unit ID */
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(4, 0x00, 1)), "write on another unit ID accepted");
	guard_for(&g, NULL, 0x1d, 4, 0x02, 0x00);			/* unknown unit */
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(4, 0x00, 1)), "unknown unit write accepted");
	EXPECT(eu_write_ok(&g, EU_REG_OUT, EU_R_SEL(0, 1)), "unknown unit page select refused");
	guard_for(&g, eu_type_lookup(0x13), 0x13, 4, 0x02, 0x00);
	g.valid = 0x1e;							/* page 0 not read */
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(4, 0x00, 1)), "write without page 0 accepted");
	EXPECT(!eu_write_ok(&g, EU_REG_EN, 0), "output enable 0 accepted");
	guard_for(&g, eu_type_lookup(0x13), 0x13, 4, 0x02, 0x00);
	g.writable = false;					/* not a table row */
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(4, 0x00, 1)), "non-writable PMP write accepted");
	guard_for(&g, eu_type_lookup(0x13), 0x13, -1, 0x02, 0x00);	/* nothing armed */
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(7, 0x00, 1)), "unarmed data word accepted");
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_DATA(4, 0x00, 1)), "unarmed page 4 word accepted");

	/* the new bytes the driver computes */
	for (d = 0; d < 256; d++) {
		for (v = 0; v < 4; v++) {
			u8 n = eu_p4_new(d, v);

			EXPECT((n & 3) == v && !(n & 0x10) && (n & ~0x13u) == (d & ~0x13u),
			       "p4_new(0x%02x, %u) = 0x%02x", d, v, n);
		}
		for (ti = 0; ti < EU_NTYPES; ti++) {
			u8 m = eu_type_p1_mask(&eu_types[ti]);
			u8 n = eu_p1_new(d, m, 0xff, (u8)~d);

			EXPECT(!(n & 0x20) && !((n ^ d) & ~m & ~0x20),
			       "p1_new(0x%02x) = 0x%02x for %s", d, n, eu_types[ti].name);
		}
	}
}

/* identification first, power-on state, polarity (review of the write order) */
static void test_identify(void)
{
	static const u32 ens[] = { 0xfffff, 0xfc7c0, 0x107c0 };
	static const u8 p0s[] = { 0x13, 0x33, 0x15, 0x1d, 0x00, 0xcc };
	unsigned int ti, pi, po, so, ei, vi;
	struct eu_guard g;
	u32 v;

	for (ti = 0; ti <= EU_NTYPES; ti++) {
		const struct eu_type *t = ti < EU_NTYPES ? &eu_types[ti] : NULL;

		for (pi = 0; pi < sizeof(p0s); pi++)
			for (vi = 0; vi < 2; vi++)
				for (po = 0; po < 2; po++)
					for (so = 0; so < 2; so++)
						for (ei = 0; ei < 3; ei++) {
							guard_for(&g, t, p0s[pi], -1, 0, 0);
							g.valid = vi ? 0x1f : 0x1e;
							g.poweron = po;
							g.setup_ok = so;
							g.en_now = ens[ei];
							for (v = 0; v < (1u << 20); v += (v & 0xfff) ? 0x1001 : 1) {
								if (eu_write_ok(&g, EU_REG_OUT, v) != ref_write_ok(&g, EU_REG_OUT, v) ||
								    eu_write_ok(&g, EU_REG_EN, v) != ref_write_ok(&g, EU_REG_EN, v) ||
								    eu_write_ok(&g, EU_REG_POL, v) != ref_write_ok(&g, EU_REG_POL, v)) {
									EXPECT(0, "%s p0 0x%02x valid %u poweron %u setup %u en 0x%05x word 0x%05x",
									       t ? t->name : "none", p0s[pi], vi, po, so, ens[ei], v);
									goto next;
								}
							}
							checks++;
next:
							;
						}
	}

	/* concrete cases */
	guard_for(&g, NULL, 0, -1, 0, 0);
	g.valid = 0;						/* nothing read yet */
	EXPECT(!eu_write_ok(&g, EU_REG_POL, EU_POL_ALL), "polarity before page 0 accepted");
	g.valid = 1;
	g.page[0] = 0x1d;					/* not a table unit ID */
	EXPECT(!eu_write_ok(&g, EU_REG_POL, EU_POL_ALL), "polarity on an unknown unit accepted");
	g.page[0] = 0x33;					/* DX1215 slave */
	EXPECT(eu_write_ok(&g, EU_REG_POL, EU_POL_ALL), "polarity on a DX1215 slave refused");
	g.id = 0x15;						/* the request wants a DX517 */
	EXPECT(!eu_write_ok(&g, EU_REG_POL, EU_POL_ALL), "polarity on another row accepted");
	guard_for(&g, NULL, 0, -1, 0, 0);
	g.poweron = true;
	g.en_now = EU_EN_POWERON;				/* power-on, no set-up allowed */
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_IDLE) && !eu_write_ok(&g, EU_REG_EN, EU_EN_ALL_OUT) &&
	       !eu_write_ok(&g, EU_REG_OUT, EU_R_SEL(0, 1)), "power-on PMP written without set-up");
	g.setup_ok = true;
	EXPECT(eu_write_ok(&g, EU_REG_OUT, EU_R_IDLE) && eu_write_ok(&g, EU_REG_EN, EU_EN_ALL_OUT),
	       "set-up refused");
	EXPECT(!eu_write_ok(&g, EU_REG_OUT, EU_R_SEL(0, 1)), "page select while strobes are inputs");
	EXPECT(eu_write_ok(&g, EU_REG_OUT, 0) && eu_write_ok(&g, EU_REG_EN, EU_EN_POWERON),
	       "back to the power-on state refused");
	g.poweron = false;
	g.en_now = EU_EN_ALL_OUT;
	EXPECT(!eu_write_ok(&g, EU_REG_EN, EU_EN_POWERON) && !eu_write_ok(&g, EU_REG_OUT, 0),
	       "power-on state written on a PMP found set up");

	/* polarity decode: measured, page 0 0x33 read as 0xcc before 0x3ac was set */
	for (v = 0; v < 256; v++) {
		u32 raw = EU_R_DATA(0, v, 0), inv = raw ^ 0xfffff;

		EXPECT(eu_dec_in(raw, EU_POL_ALL) == v && eu_dec_in(inv, 0) == v,
		       "DEC_IN 0x%02x", v);
	}
	EXPECT(EU_DEC(EU_R_DATA(0, 0x33, 0) ^ 0xfffff) == 0xcc, "inverted 0x33 is 0xcc");
	EXPECT(eu_pol_known(0) && eu_pol_known(0xfffff) && !eu_pol_known(0x107c0) &&
	       !eu_pol_known(1), "known polarity values");
}

/* rows not confirmed: no fan or LED writes unless allowed; LED writes only on change */
static void test_policy(void)
{
	unsigned int i, cur, m, w;

	for (i = 0; i < EU_NTYPES; i++) {
		const struct eu_type *t = &eu_types[i];

		EXPECT(eu_type_writable(t, false) == (t->id == 0x13), "%s writable by default", t->name);
		EXPECT(eu_type_writable(t, true), "%s writable with the parameter", t->name);
	}
	EXPECT(!eu_type_writable(NULL, true), "unknown unit writable");
	for (i = 0; i < EU_NTYPES; i++) {
		u8 pm = eu_type_p1_mask(&eu_types[i]);

		for (cur = 0; cur < 256; cur++)
			for (m = 0; m < 256; m += 3)
				for (w = 0; w < 256; w += 5) {
					u8 d = eu_p1_diff(cur, pm, m, w);
					u8 want = (cur ^ w) & m & pm & ~0x20;

					if (d != want) {
						EXPECT(0, "p1_diff(0x%02x, 0x%02x, 0x%02x, 0x%02x) = 0x%02x",
						       cur, pm, m, w, d);
						goto out;
					}
					/* nothing to write when the bits already have the value */
					if (!((cur ^ w) & m & pm) && d) {
						EXPECT(0, "p1_diff writes an unchanged bit");
						goto out;
					}
				}
	}
	checks++;
out:
	EXPECT(eu_p1_diff(0x40, 0x47, 0x02, 0x02) == 0x02, "bay 8 LED on");
	EXPECT(eu_p1_diff(0x42, 0x47, 0x02, 0x02) == 0x00, "bay 8 LED already on");
}

int main(void)
{
	test_encoding();
	test_table();
	test_guard();
	test_identify();
	test_policy();
	printf("%lu checks, %lu failed\n", checks, fails);
	return fails ? 1 : 0;
}
