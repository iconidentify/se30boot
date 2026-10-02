/* mmu.c - where Mac OS's logical memory really is.
 *
 * Mac OS runs with the 68030 MMU on, and on an SE/30 with different-sized
 * banks the RAM is not where it appears: with 4 MB in bank A and 64 MB in
 * bank B, logical 4 MB is physical 0x04000000. Linux needs the physical
 * memory map, and the final copy of the kernel runs with the MMU off, so
 * stage 2 walks Mac OS's translation tables as the MMU would.
 *
 * The tables hold physical addresses. Like Penguin, a tiny routine in low
 * memory (where logical and physical addresses are the same on everything
 * but RBV machines) reads them through transparent translation register 0,
 * with interrupts and the caches off. */
#include "boot.h"

struct mmu_regs mmu;
struct bank bank[MAXBANKS];
int nbanks;
struct chunk chunk[MAXCHUNKS];
int nchunks;

/* start.S */
void mmu_get(struct mmu_regs *m);
u32 phys_read(u32 addr);
void phys_begin(void);
void phys_end(void);

static int tt_match(u32 tt, u32 log)
{
	u32 base = tt >> 24, mask = tt >> 16 & 0xff;

	return (tt & 0x8000) && !(((log >> 24) ^ base) & ~mask);
}

static u32 low_mask(int bits)
{
	return bits >= 32 ? 0xffffffff : (1u << bits) - 1;
}

/* Translate log. Returns 1 and the physical address if it is mapped, 0 if
   not. *span is how far the answer holds: the rest of the page, or of the
   invalid region, from log. */
int mmu_translate(u32 log, u32 *phys, u32 *span)
{
	int ti[4], used, lvl;
	u32 a, dt, table, d0, d1, base;
	const u32 *root;

	if (!(mmu.tc & 0x80000000) || tt_match(mmu.tt0, log) || tt_match(mmu.tt1, log)) {
		*phys = log;
		*span = 0x1000000 - (log & 0xffffff);
		return 1;
	}
	root = (mmu.tc & 0x02000000) ? mmu.srp : mmu.crp;
	ti[0] = mmu.tc >> 12 & 15;
	ti[1] = mmu.tc >> 8 & 15;
	ti[2] = mmu.tc >> 4 & 15;
	ti[3] = mmu.tc & 15;
	used = mmu.tc >> 16 & 15;		/* IS */
	a = used ? log << used : log;
	dt = root[0] & 3;
	table = root[1] & ~15u;
	if (dt == 1) {				/* the whole space is one page */
		base = root[1] & ~0xffu;
		goto page;
	}
	for (lvl = 0; lvl < 4 && ti[lvl]; lvl++) {
		int n = ti[lvl];
		u32 idx = a >> (32 - n), ddt;

		a <<= n;
		used += n;
		if (dt == 2) {
			d0 = phys_read(table + idx * 4);
			ddt = d0 & 3;
			table = d0 & ~15u;
			base = d0 & ~0xffu;
		} else {
			d0 = phys_read(table + idx * 8);
			d1 = phys_read(table + idx * 8 + 4);
			ddt = d0 & 3;
			table = d1 & ~15u;
			base = d1 & ~0xffu;
		}
		if (ddt == 0) {			/* invalid */
			*span = low_mask(32 - used) - (log & low_mask(32 - used)) + 1;
			return 0;
		}
		if (ddt == 1)			/* a page, maybe early termination */
			goto page;
		if (lvl == 3 || !ti[lvl + 1]) {	/* indirect, to a page descriptor */
			if (ddt == 2) {
				d0 = phys_read(table);
				base = d0 & ~0xffu;
			} else {
				d0 = phys_read(table);
				base = phys_read(table + 4) & ~0xffu;
			}
			if ((d0 & 3) != 1)
				return *span = low_mask(32 - used) + 1 - (log & low_mask(32 - used)), 0;
			goto page;
		}
		dt = ddt;
	}
	*span = 0x8000;
	return 0;
page:
	*phys = base + (log & low_mask(32 - used));
	*span = low_mask(32 - used) - (log & low_mask(32 - used)) + 1;
	return 1;
}

static void bank_add(u32 log, u32 phys, u32 size)
{
	if (nbanks && bank[nbanks - 1].log + bank[nbanks - 1].size == log &&
	    bank[nbanks - 1].phys + bank[nbanks - 1].size == phys) {
		bank[nbanks - 1].size += size;
		return;
	}
	if (nbanks < MAXBANKS) {
		bank[nbanks].log = log;
		bank[nbanks].phys = phys;
		bank[nbanks].size = size;
		nbanks++;
	}
}

/* Merge a physical range into the chunk list, which stays sorted by
   address with no overlaps. */
static void chunk_add(u32 addr, u32 size)
{
	int i, j;
	u32 end = addr + size;

	for (i = 0; i < nchunks; i++) {
		u32 cend = chunk[i].addr + chunk[i].size;

		if (end < chunk[i].addr)
			break;
		if (addr <= cend) {		/* touches or overlaps: merge */
			if (addr > chunk[i].addr)
				addr = chunk[i].addr;
			if (end < cend)
				end = cend;
			for (j = i; j < nchunks - 1; j++)
				chunk[j] = chunk[j + 1];
			nchunks--;
			i--;
		}
	}
	if (nchunks == MAXCHUNKS)
		return;
	for (j = nchunks; j > i; j--)
		chunk[j] = chunk[j - 1];
	chunk[i].addr = addr;
	chunk[i].size = end - addr;
	nchunks++;
}

/* The top of logical RAM. In 24-bit mode MemTop is all of the RAM, but
   only the first 8 MB is addressable; above it are the ROM and the slots. */
u32 mem_top(void)
{
	u32 top = MemTop;

	if (!LM8(0xcb2) && top > 0x800000)	/* MMU32Bit: 24-bit mode */
		top = 0x800000;
	return top;
}

/* Walk logical RAM into banks (logical to physical runs) and chunks
   (physical RAM). */
int mmu_scan(void)
{
	u32 log = 0, top = mem_top(), phys, span;

	mmu_get(&mmu);
	nbanks = nchunks = 0;
	phys_begin();
	while (log < top) {
		int ok = mmu_translate(log, &phys, &span);

		if (span > top - log)
			span = top - log;
		if (ok && phys < 0x40000000) {		/* RAM, not ROM or I/O */
			bank_add(log, phys, span);
			chunk_add(phys, span);
		}
		log += span;
	}
	phys_end();
	return nbanks ? 0 : -1;
}

/* one translation outside the scan */
int log2phys(u32 log, u32 *phys)
{
	u32 span;
	int ok;

	phys_begin();
	ok = mmu_translate(log, phys, &span);
	phys_end();
	return ok ? 0 : -1;
}

/* How many bytes from log on are both logically and physically
   contiguous RAM (0 if log isn't RAM). */
u32 contiguous_at(u32 log)
{
	int i;

	for (i = 0; i < nbanks; i++)
		if (log >= bank[i].log && log - bank[i].log < bank[i].size)
			return bank[i].size - (log - bank[i].log);
	return 0;
}

u32 bank_phys(u32 log)
{
	int i;

	for (i = 0; i < nbanks; i++)
		if (log >= bank[i].log && log - bank[i].log < bank[i].size)
			return bank[i].phys + (log - bank[i].log);
	return 0xffffffff;
}
