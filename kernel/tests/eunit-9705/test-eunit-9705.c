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

static bool ref_write_ok(const struct eu_guard *g, u16 reg, u32 val)
{
	u32 sel_bits = (1u << 14) | (1u << 15) | (7u << 17);

	if (reg == 0x3a4)
		return val == 0xfffff || val == 0xfc7c0;
	if (reg != 0x3a0)
		return false;
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
	EXPECT(EU_REG_OUT == 0x3a0 && EU_REG_EN == 0x3a4 && EU_REG_IN == 0x3b0, "registers");
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

int main(void)
{
	test_encoding();
	test_table();
	test_guard();
	printf("%lu checks, %lu failed\n", checks, fails);
	return fails ? 1 : 0;
}
