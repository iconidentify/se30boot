/* load.c - load a kernel the way Penguin does, without Mac OS.
 *
 * The kernel is read from the boot volume and unpacked as a stream, each
 * ELF segment going straight to its place in one buffer that is physically
 * contiguous: the image Penguin would build, with the bootinfo records
 * after it. Then the final copy runs from low memory with the MMU off. */
#include "boot.h"

#define PAGE_SIZE	0x1000
#define BI_SIZE		0x1000		/* room for the bootinfo records */
#define KERNEL_ALIGN	0x40000		/* Penguin aligns the kernel's chunk */

/* --- memory: the free parts of Mac OS's logical RAM --- */

/* Free ranges, in address order: everything above the heap Mac OS has so
   far, less the ROM's stack and our boot blocks (which Mac OS still needs
   if it is chosen), stage 2, what lies above BufPtr below 8 MB, and the
   top megabyte, where the ROM keeps its MMU tables. At startup the ROM
   puts the stack at 4 MB, so in 24-bit mode the free RAM comes in pieces
   of a few MB on either side of it. */
#define MAXFREE 8
static struct { u32 lo, hi; } freer[MAXFREE];
static int nfree;

static void cut(u32 lo, u32 hi)
{
	int i, j;

	lo &= ~0xfff;			/* whole pages, so pieces are too */
	hi = (hi + 0xfff) & ~0xfff;
	for (i = 0; i < nfree; i++) {
		if (hi <= freer[i].lo || lo >= freer[i].hi)
			continue;
		if (lo > freer[i].lo && hi < freer[i].hi && nfree < MAXFREE) {
			for (j = nfree; j > i + 1; j--)	/* split in two */
				freer[j] = freer[j - 1];
			freer[i + 1].lo = hi;
			freer[i + 1].hi = freer[i].hi;
			freer[i].hi = lo;
			nfree++;
			i++;
		} else if (lo > freer[i].lo) {
			freer[i].hi = lo;
		} else if (hi < freer[i].hi) {
			freer[i].lo = hi;
		} else {
			for (j = i; j < nfree - 1; j++)
				freer[j] = freer[j + 1];
			nfree--;
			i--;
		}
	}
}

void heap_init(void)
{
	u32 sp = get_sp(), top = mem_top();

	nfree = 1;
	freer[0].lo = (HeapEnd + 0x10000 + 0xfff) & ~0xfff;
	freer[0].hi = (top > 0x1000000 ? top - 0x100000 : top) & ~0xfff;
	cut(sp - 0x10000, sp + 0x2000);
	cut(bootblock & ~0xfff, (bootblock + 0x1400) & ~0xfff);
	cut(SAVE_AREA & ~0xffff, STAGE2_TOP);
	if (BufPtr < 0x800000)
		cut(BufPtr & ~0xfff, 0x800000);
}

/* small things: from the top of the highest range with room */
u8 *heap_alloc(u32 size)
{
	int i;

	size = (size + 511) & ~511;
	for (i = nfree - 1; i >= 0; i--)
		if (freer[i].hi - freer[i].lo >= size) {
			freer[i].hi -= size;
			return (u8 *)freer[i].hi;
		}
	return NULL;
}

u32 heap_free(void)
{
	u32 n = 0;
	int i;

	for (i = 0; i < nfree; i++)
		n += freer[i].hi - freer[i].lo;
	return n;
}

/* The kernel image: pieces of logical memory, each physically contiguous,
   in address order. The mover copies them one after another to where the
   kernel runs. */
#define MAXPIECE 8
static struct piece { u32 log, phys, len; } img[MAXPIECE];
static int nimg;

/* One piece if any free range has a run that long, the highest first:
   one piece can always be moved into place. Otherwise pieces from the
   bottom up, which suits a kernel that goes to the bottom of memory. */
static int alloc_image(u32 size)
{
	int i;

	nimg = 0;
	for (i = nfree - 1; i >= 0; i--) {
		u32 a = (freer[i].lo + 0xfff) & ~0xfff;

		while (a + size <= freer[i].hi) {
			u32 run = contiguous_at(a);

			if (run >= size) {
				img[0].log = a;
				img[0].phys = bank_phys(a);
				img[0].len = size;
				nimg = 1;
				freer[i].lo = a + size;
				return 0;
			}
			a += run ? run : 0x1000;
		}
	}
	for (i = 0; i < nfree && size; i++) {
		u32 a = (freer[i].lo + 0xfff) & ~0xfff;

		while (a < freer[i].hi && size) {
			u32 run = contiguous_at(a), n;

			if (!run) {
				a += 0x1000;
				continue;
			}
			n = freer[i].hi - a;
			if (n > run)
				n = run;
			if (n > size)
				n = size;
			if (nimg == MAXPIECE)
				return -1;
			img[nimg].log = a;
			img[nimg].phys = bank_phys(a);
			img[nimg].len = n;
			nimg++;
			size -= n;
			a += n;
			freer[i].lo = a;
		}
	}
	return size ? -1 : 0;
}

/* copy into or clear the image, at an offset */
static void image_write(u32 off, const u8 *data, u32 len)
{
	int i;

	for (i = 0; i < nimg && len; i++) {
		if (off < img[i].len) {
			u32 n = img[i].len - off;

			if (n > len)
				n = len;
			if (data) {
				memcpy((u8 *)img[i].log + off, data, n);
				data += n;
			} else {
				memset((u8 *)img[i].log + off, 0, n);
			}
			len -= n;
			off = 0;
		} else {
			off -= img[i].len;
		}
	}
}

/* --- reading the kernel file, piece by piece --- */

#define PIECE 0x10000
static struct hfs_file kf;
static u32 kpos;
static u8 *piece;

static u32 disk_ticks;		/* time spent reading, for "debug" */

static int fill(const u8 **buf, u32 *len)
{
	u32 n = kf.size - kpos, t = Ticks;

	if (!n)
		return -1;
	if (n > PIECE)
		n = PIECE;
	if (hfs_read(&kf, kpos, n, piece, NULL))
		return -1;
	kpos += n;
	*buf = piece;
	*len = n;
	progress_bar(kpos, kf.size);
	disk_ticks += Ticks - t;
	return 0;
}

/* --- placing ELF segments as the unpacked bytes stream past --- */

#define MAXSEG 8
static struct {
	u8 hdr[PAGE_SIZE];		/* the ELF and program headers */
	u32 hdrlen;
	int ready;
	struct { u32 off, filesz, dst, memsz; } seg[MAXSEG];
	int nseg;
	u32 min_addr, max_addr, entry, placed, needed;
	u32 size;			/* max_addr - min_addr */
	const char *err;
} elf;

static u16 be16(const u8 *p) { return p[0] << 8 | p[1]; }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | p[2] << 8 | p[3]; }

static void place(u32 off, const u8 *data, u32 len)
{
	int i;

	for (i = 0; i < elf.nseg; i++) {
		u32 lo = off, hi = off + len;

		if (lo < elf.seg[i].off)
			lo = elf.seg[i].off;
		if (hi > elf.seg[i].off + elf.seg[i].filesz)
			hi = elf.seg[i].off + elf.seg[i].filesz;
		if (lo < hi) {
			image_write(elf.seg[i].dst + (lo - elf.seg[i].off),
				    data + (lo - off), hi - lo);
			elf.placed += hi - lo;
		}
	}
}

/* Read the headers. Like Penguin: every PT_LOAD segment, with the first
   page dropped if the linker put the headers in the first segment. */
static int parse_headers(void)
{
	const u8 *h = elf.hdr;
	u32 phoff, i, n;

	if (elf.hdrlen < 52 || be32(h) != 0x7f454c46 || h[4] != 1 || h[5] != 2)
		return elf.err = "not an ELF file for a 68k", -1;
	if (be16(h + 16) != 2 || be16(h + 18) != 4)
		return elf.err = "not a 68k executable", -1;
	phoff = be32(h + 28);
	n = be16(h + 44);
	if (phoff + n * 32 > elf.hdrlen)
		return elf.err = "program headers are too far in", -1;
	elf.entry = be32(h + 24);
	elf.min_addr = 0xffffffff;
	elf.max_addr = 0;
	elf.nseg = 0;
	for (i = 0; i < n; i++) {
		const u8 *ph = h + phoff + i * 32;
		u32 vaddr = be32(ph + 8), memsz = be32(ph + 20);

		if (be32(ph) != 1)		/* PT_LOAD */
			continue;
		if (elf.nseg == MAXSEG)
			return elf.err = "too many segments", -1;
		elf.seg[elf.nseg].off = be32(ph + 4);
		elf.seg[elf.nseg].filesz = be32(ph + 16);
		elf.seg[elf.nseg].dst = vaddr;
		elf.seg[elf.nseg].memsz = memsz;
		if (vaddr < elf.min_addr)
			elf.min_addr = vaddr;
		if (vaddr + memsz > elf.max_addr)
			elf.max_addr = vaddr + memsz;
		elf.nseg++;
	}
	if (!elf.nseg)
		return elf.err = "nothing to load", -1;
	if (elf.min_addr == 0) {
		for (i = 0; i < (u32)elf.nseg; i++)
			if (elf.seg[i].dst == 0) {
				elf.seg[i].dst += PAGE_SIZE;
				elf.seg[i].off += PAGE_SIZE;
				elf.seg[i].filesz -= PAGE_SIZE;
				elf.seg[i].memsz -= PAGE_SIZE;
			}
		elf.min_addr = PAGE_SIZE;
	}
	elf.needed = 0;
	for (i = 0; i < (u32)elf.nseg; i++) {
		elf.seg[i].dst -= elf.min_addr;
		elf.needed += elf.seg[i].filesz;
	}
	elf.size = (elf.max_addr - elf.min_addr + 3) & ~3;
	if (alloc_image(elf.size + BI_SIZE))
		return elf.err = "not enough memory for the kernel", -1;
	image_write(0, NULL, elf.size + BI_SIZE);
	elf.ready = 1;
	place(0, elf.hdr, elf.hdrlen);
	return 0;
}

static int out(u32 off, const u8 *data, u32 len)
{
	if (!elf.ready) {
		u32 n = PAGE_SIZE - elf.hdrlen;

		if (n > len)
			n = len;
		memcpy(elf.hdr + elf.hdrlen, data, n);
		elf.hdrlen += n;
		off += n;
		data += n;
		len -= n;
		if (elf.hdrlen < PAGE_SIZE)
			return 0;
		if (parse_headers())
			return -1;
	}
	place(off, data, len);
	return 0;
}

/* an uncompressed kernel goes through the same placing */
static int raw(void)
{
	const u8 *buf;
	u32 len, off = 0;

	while (!fill(&buf, &len)) {
		if (out(off, buf, len))
			return -1;
		off += len;
	}
	if (!elf.ready && parse_headers())
		return -1;
	return 0;
}

/* --- bootinfo --- */

static u8 bibuf[BI_SIZE], *bi;

static void bi_add(u16 tag, const void *data, u32 len)
{
	u16 size = (4 + len + 3) & ~3;

	if (bi + size + 4 > bibuf + BI_SIZE)
		return;
	bi[0] = tag >> 8;
	bi[1] = tag;
	bi[2] = size >> 8;
	bi[3] = size;
	memset(bi + 4, 0, size - 4);
	memcpy(bi + 4, data, len);
	bi += size;
}

static void bi_long(u16 tag, u32 v)
{
	bi_add(tag, &v, 4);
}

/* physical address of a logical one, or a fallback */
static u32 phys_or(u32 log, u32 fallback)
{
	u32 p;

	return log2phys(log, &p) ? fallback : p;
}

/* the FPU, as Linux names it: 1 = 68881, 2 = 68882, 0 none */
int fpu_type;

void fpu_detect(void)
{
	int size = fpu_probe();

	fpu_type = size < 0 ? 0 : size == 0x18 ? 1 : 2;
}

static void make_bootinfo(const struct chunk *mem, int nmem, const char *cmdline)
{
	u8 loc[12];
	s32 gmt;
	u32 scc = SCCRd, total = 0;
	int i, w = 512, h = 342;

	bi = bibuf;
	bi_long(0x0001, 3);			/* BI_MACHTYPE: MACH_MAC */
	bi_long(0x0002, 2);			/* BI_CPUTYPE: 68030 */
	bi_long(0x0003, fpu_type);		/* BI_FPUTYPE */
	bi_long(0x0004, 2);			/* BI_MMUTYPE: 68030 */
	for (i = 0; i < nmem; i++) {
		bi_add(0x0005, &mem[i], 8);	/* BI_MEMCHUNK */
		total += mem[i].size;
	}
	bi_add(0x0007, cmdline, strlen(cmdline) + 1);	/* BI_COMMAND_LINE */
	bi_long(0x8000, 9);			/* BI_MAC_MODEL: SE/30 */
	bi_long(0x8001, phys_or(ScrnBase, 0xfe008040));	/* BI_MAC_VADDR */
	bi_long(0x8002, 1);			/* BI_MAC_VDEPTH */
	bi_long(0x8003, ScreenRow);		/* BI_MAC_VROW */
	if (CrsrPin[2] > CrsrPin[0] && CrsrPin[3] > CrsrPin[1]) {
		w = CrsrPin[3] - CrsrPin[1];
		h = CrsrPin[2] - CrsrPin[0];
	}
	bi_long(0x8004, h << 16 | w);		/* BI_MAC_VDIM */
	bi_long(0x8005, ScrnBase);		/* BI_MAC_VLOGICAL */
	if (!(scc >> 24))			/* 24-bit: the I/O space is at 0x50000000 */
		scc |= 0x50000000;
	bi_long(0x8006, phys_or(SCCRd, scc));	/* BI_MAC_SCCBASE */
	bi_long(0x8007, Time - 2082844800u);	/* BI_MAC_BTIME, as a Unix time */
	rom_readxpram(loc, 0xe4, 12);		/* the Map control panel's location */
	gmt = (s32)(loc[9] << 24 | loc[10] << 16 | loc[11] << 8) >> 8;
	bi_long(0x8008, gmt / 60);		/* BI_MAC_GMTBIAS, minutes */
	bi_long(0x8009, total >> 20);		/* BI_MAC_MEMSIZE, MB */
	bi_long(0x800a, 1);			/* BI_MAC_CPUID: 68030 */
	bi_long(0x800b, phys_or(ROMBase, 0x40800000));	/* BI_MAC_ROMBASE */
	bi_add(0x0000, NULL, 0);		/* BI_LAST */
	image_write(elf.size, bibuf, BI_SIZE);
}

/* --- leaving Mac OS --- */

/* The mover copies the pieces in order, each forwards if it moves down and
   backwards if up. That is safe if no piece's destination covers the
   source of a piece not yet copied. */
static u32 load_start;
int debug;			/* "debug" in the configuration */

static const char *go(u32 dst, u32 entry, const char *title)
{
	u32 d = dst;
	int i, j;

	for (i = 0; i < nimg; i++) {
		for (j = i + 1; j < nimg; j++)
			if (d < img[j].phys + img[j].len && img[j].phys < d + img[i].len)
				return "can't place the kernel safely (memory is too fragmented)";
		if (img[i].phys == 0xffffffff)
			return "lost track of the kernel's memory";
		d += img[i].len;
	}
	if (bank_phys(MOVER) != MOVER)
		return "low memory isn't mapped one to one";
	message("Starting %s (%u KB loaded in %u s)", title, elf.size >> 10,
		(Ticks - load_start + 30) / 60);
	if (debug) {
		con_printf(2, 3 + MAXENTRIES + nimg + 1, 0, "FPU %s; RAM %u chunk%s from %p; disk %u s",
			   fpu_type == 2 ? "68882" : fpu_type == 1 ? "68881" : "none",
			   nchunks, nchunks == 1 ? "" : "s", chunk[0].addr, disk_ticks / 60);
		for (i = 0; i < nimg; i++)
			con_printf(2, 3 + MAXENTRIES + i, 0, "piece %d: %p (physical %p) + %u",
				   i, img[i].log, img[i].phys, img[i].len);
		con_printf(2, 3 + MAXENTRIES + i, 0, "to %p, entry %p; a key starts it", dst, entry);
		wait_key();
	}
	go_kernel(img, nimg, dst, entry);
}

/* --- putting it together --- */

/* the "memory" lines of the configuration, if any, replace the RAM found */
struct chunk mem_override[MAXCHUNKS];
int nmem_override;

const char *load_kernel(const struct entry *e)
{
	static char why[96];
	struct chunk mem[MAXCHUNKS];
	u32 len, dst, i, j;
	int nmem, err;

	if (hfs_lookup(e->kernel, &kf))
		return snprintf_(why, sizeof(why), "can't find %s", e->kernel), why;
	/* Only now look at Mac OS's MMU tables and the FPU, so that nothing
	   but the disk and the screen stands between the menu and Mac OS. */
	if (!nchunks) {
		message("Reading the memory map");
		if (mmu_scan())
			return "can't read Mac OS's memory map";
		fpu_detect();
	}
	if (!piece && !(piece = heap_alloc(PIECE)))
		return "out of memory";
	memset(&elf, 0, sizeof(elf));
	kpos = 0;
	load_start = Ticks;
	disk_ticks = 0;
	message("Loading %s (%u KB)", e->kernel, kf.size >> 10);

	/* gzip, or an ELF file as it is */
	if (hfs_read(&kf, 0, 4, piece, NULL))
		return snprintf_(why, sizeof(why), "disk error %d", hfs_error), why;
	if (piece[0] == 0x1f && piece[1] == 0x8b) {
		err = gunzip(fill, out, &len);
		if (err)
			return elf.err ? elf.err : gunzip_error(err);
	} else if (raw()) {
		return elf.err ? elf.err : "read error";
	}
	if (!elf.ready && !elf.err)
		parse_headers();
	if (elf.err)
		return elf.err;
	if (elf.placed != elf.needed)
		return "the kernel file is shorter than its headers say";

	/* Penguin's memory list: largest chunk first, the kernel 4 KB into it */
	nmem = nmem_override ? nmem_override : nchunks;
	for (i = 0; i < (u32)nmem; i++)
		mem[i] = nmem_override ? mem_override[i] : chunk[i];
	for (i = 0; i < (u32)nmem; i++)
		for (j = i + 1; j < (u32)nmem; j++)
			if (mem[j].size > mem[i].size) {
				struct chunk t = mem[i];

				mem[i] = mem[j];
				mem[j] = t;
			}
	if (mem[0].addr & (KERNEL_ALIGN - 1)) {
		u32 skip = KERNEL_ALIGN - (mem[0].addr & (KERNEL_ALIGN - 1));

		mem[0].addr += skip;
		mem[0].size -= skip;
	}
	if (elf.size + BI_SIZE + PAGE_SIZE > mem[0].size)
		return "the kernel doesn't fit in the largest memory bank";
	dst = mem[0].addr + PAGE_SIZE;
	make_bootinfo(mem, nmem, e->append);

	return go(dst, dst + (elf.entry - elf.min_addr), e->title);
}
