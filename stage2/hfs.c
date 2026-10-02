/* hfs.c - read files from the boot volume through the ROM's disk driver.
 *
 * The Mac OS driver for the boot disk is still installed, so _Read on the
 * boot drive gives us the HFS volume. Lookups walk the B-trees' leaf
 * chains, reading nodes as needed through a small cache, which avoids
 * depending on HFS's Mac Roman key ordering; a few hundred files is a few
 * dozen nodes. */
#include "boot.h"

int hfs_error;			/* the last _Read result */

static s16 drive, refnum;

/* the Device Manager's parameter block */
struct iopb {
	u32 link;
	u16 type, trap;
	u32 cmd, completion;
	s16 result;
	u32 name;
	s16 vrefnum, refnum;
	u8 version, perm;
	u32 misc, buffer, count, actual;
	u16 posmode;
	u32 posoffset;
} __attribute__((packed));

int disk_init(void)
{
	u32 q = LM32(0x30a);		/* DrvQHdr.qHead */
	s16 boot = LM16(0x210);		/* BootDrive */

	while (q) {
		if (*(s16 *)(q + 6) == boot) {
			drive = boot;
			refnum = *(s16 *)(q + 8);
			return 0;
		}
		q = *(u32 *)q;
	}
	return -1;
}

/* off and len in bytes, both multiples of 512 */
static int disk_read_blocks(u32 off, u32 len, void *buf)
{
	struct iopb pb;

	memset(&pb, 0, sizeof(pb));
	pb.vrefnum = drive;
	pb.refnum = refnum;
	pb.buffer = (u32)buf;
	pb.count = len;
	pb.posmode = 1;			/* fsFromStart */
	pb.posoffset = off;
	hfs_error = rom_read(&pb);
	return hfs_error ? -1 : 0;
}

static u8 bounce[512];

/* any offset and length */
int disk_read(u32 off, u32 len, void *buf)
{
	u8 *p = buf;

	while (len) {
		u32 skip = off & 511, n;

		if (skip || len < 512) {
			n = 512 - skip;
			if (n > len)
				n = len;
			if (disk_read_blocks(off - skip, 512, bounce))
				return -1;
			memcpy(p, bounce + skip, n);
		} else {
			n = len & ~511;
			if (n > 0x10000)
				n = 0x10000;
			if (disk_read_blocks(off, n, p))
				return -1;
		}
		off += n;
		p += n;
		len -= n;
	}
	return 0;
}

static u16 be16(const u8 *p) { return p[0] << 8 | p[1]; }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | p[2] << 8 | p[3]; }

static u32 blk_size, blk_start;		/* allocation blocks */
static u8 mdb[512];

/* an extent list: up to 3 from the MDB or catalog plus overflow records */
#define MAXEXT 64
struct extlist {
	int n;
	u16 start[MAXEXT], count[MAXEXT];
};

static void ext_add3(struct extlist *x, const u8 *rec)
{
	int i;

	for (i = 0; i < 3 && x->n < MAXEXT; i++) {
		u16 c = be16(rec + i * 4 + 2);

		if (!c)
			break;
		x->start[x->n] = be16(rec + i * 4);
		x->count[x->n] = c;
		x->n++;
	}
}

static u32 ext_blocks(const struct extlist *x)
{
	u32 n = 0;
	int i;

	for (i = 0; i < x->n; i++)
		n += x->count[i];
	return n;
}

static int ext_read(const struct extlist *x, u32 off, u32 len, void *buf,
		    void (*progress)(u32 done, u32 total));

/* the two B-trees, and a cache of their nodes */
struct btree {
	int id;
	u32 size;
	struct extlist x;
};
static struct btree catalog, extents;

#define NCACHE 64
static u8 cache[NCACHE][512];
static u32 cache_tag[NCACHE];		/* tree id << 24 | node, plus 1 */

static const u8 *node(struct btree *t, u32 n)
{
	u32 tag = ((u32)t->id << 24 | n) + 1;
	int slot = n % NCACHE;

	if (cache_tag[slot] != tag) {
		if ((n + 1) * 512 > t->size || ext_read(&t->x, n * 512, 512, cache[slot], NULL))
			return NULL;
		cache_tag[slot] = tag;
	}
	return cache[slot];
}

/* Visit every record in a B-tree's leaves, in key order. */
static int btree_leaves(struct btree *t,
			int (*fn)(const u8 *key, const u8 *data, void *arg), void *arg)
{
	const u8 *nd = node(t, 0);
	u32 n;
	int guard = 0;

	if (!nd || be16(nd + 14 + 18) != 512)	/* bthNodeSize */
		return -1;
	n = be32(nd + 14 + 10);			/* bthFNode */
	while (n && guard++ < 65536) {
		int nrecs, i;

		nd = node(t, n);
		if (!nd || nd[8] != 0xff)	/* not a leaf */
			return -1;
		nrecs = be16(nd + 10);
		for (i = 0; i < nrecs; i++) {
			u16 off = be16(nd + 512 - 2 * (i + 1));
			const u8 *key = nd + off;
			const u8 *data = key + ((1 + key[0] + 1) & ~1);
			int r;

			if (off >= 512)
				return -1;
			r = fn(key, data, arg);
			if (r)
				return r;
		}
		n = be32(nd);			/* ndFLink */
	}
	return 0;
}

struct overflow_arg {
	u32 id;
	u8 fork;
	struct extlist *x;
};

static int overflow_fn(const u8 *key, const u8 *data, void *arg)
{
	struct overflow_arg *a = arg;

	if (key[0] >= 7 && key[1] == a->fork && be32(key + 2) == a->id &&
	    be16(key + 6) == ext_blocks(a->x))
		ext_add3(a->x, data);
	return 0;
}

/* complete x with the extents overflow file's records for file id */
static void ext_overflow(u32 id, struct extlist *x)
{
	struct overflow_arg a = { id, 0, x };
	int n;

	if (!extents.size)
		return;
	do {			/* records come in key order, but be safe */
		n = x->n;
		btree_leaves(&extents, overflow_fn, &a);
	} while (x->n != n && x->n < MAXEXT);
}

/* read len bytes at off within the file described by x */
static int ext_read(const struct extlist *x, u32 off, u32 len, void *buf,
		    void (*progress)(u32 done, u32 total))
{
	u8 *p = buf;
	u32 pos = 0, done = 0, total = len;
	int i;

	for (i = 0; i < x->n && len; i++) {
		u32 elen = x->count[i] * blk_size;

		if (off < pos + elen) {
			u32 eoff = off - pos, n = elen - eoff;
			u32 disk = (blk_start * 512) + x->start[i] * blk_size + eoff;

			if (n > len)
				n = len;
			while (n) {	/* in pieces, so progress can be shown */
				u32 piece = n > 0x10000 ? 0x10000 : n;

				if (disk_read(disk, piece, p))
					return -1;
				disk += piece;
				p += piece;
				off += piece;
				len -= piece;
				n -= piece;
				done += piece;
				if (progress)
					progress(done, total);
			}
		}
		pos += elen;
	}
	return len ? -1 : 0;
}

int hfs_mount(void)
{
	if (disk_init())
		return -1;
	if (disk_read(1024, 512, mdb))
		return -2;
	if (be16(mdb) != 0x4244)	/* 'BD' */
		return -3;
	blk_size = be32(mdb + 0x14);
	blk_start = be16(mdb + 0x1c);
	extents.id = 3;
	extents.size = be32(mdb + 0x82);
	ext_add3(&extents.x, mdb + 0x86);
	catalog.id = 4;
	catalog.size = be32(mdb + 0x92);
	ext_add3(&catalog.x, mdb + 0x96);
	if (ext_blocks(&catalog.x) * blk_size < catalog.size)
		ext_overflow(4, &catalog.x);
	if (!node(&catalog, 0))
		return -4;
	return 0;
}

/* the volume's name, from the MDB */
void hfs_volname(char *buf)
{
	int n = mdb[0x24];

	if (n > 27)
		n = 27;
	memcpy(buf, mdb + 0x25, n);
	buf[n] = 0;
}

struct find_arg {
	u32 parent;
	const char *name;
	int namelen;
	const u8 *found;
};

static int find_fn(const u8 *key, const u8 *data, void *arg)
{
	struct find_arg *a = arg;
	int i;

	if (key[0] < 6 || be32(key + 2) != a->parent || key[6] != a->namelen)
		return 0;
	for (i = 0; i < a->namelen; i++) {
		int c = key[7 + i], d = (u8)a->name[i];

		if (c >= 'a' && c <= 'z')
			c -= 32;
		if (d >= 'a' && d <= 'z')
			d -= 32;
		if (c != d)
			return 0;
	}
	if (data[0] != 1 && data[0] != 2)	/* a folder or a file */
		return 0;
	a->found = data;
	return 1;
}

/* "Linux:vmlinux.gz" or "Linux/vmlinux.gz", from the volume's root */
int hfs_lookup(const char *path, struct hfs_file *f)
{
	struct find_arg a;
	u32 dir = 2;			/* the root folder */
	struct extlist x;
	int i;

	while (*path == ':' || *path == '/')
		path++;
	for (;;) {
		const char *end = path;

		while (*end && *end != ':' && *end != '/')
			end++;
		a.parent = dir;
		a.name = path;
		a.namelen = end - path;
		a.found = NULL;
		btree_leaves(&catalog, find_fn, &a);
		if (!a.found)
			return -1;
		while (*end == ':' || *end == '/')
			end++;
		if (!*end) {
			if (a.found[0] != 2)	/* a folder */
				return -2;
			break;
		}
		if (a.found[0] != 1)
			return -1;
		dir = be32(a.found + 6);	/* dirDirID */
		path = end;
	}
	f->id = be32(a.found + 0x14);		/* filFlNum */
	f->size = be32(a.found + 0x1a);		/* filLgLen */
	x.n = 0;
	ext_add3(&x, a.found + 0x4a);		/* filExtRec */
	for (i = 0; i < 3; i++) {
		f->ext[i][0] = i < x.n ? x.start[i] : 0;
		f->ext[i][1] = i < x.n ? x.count[i] : 0;
	}
	return 0;
}

int hfs_read(const struct hfs_file *f, u32 off, u32 len, void *buf,
	     void (*progress)(u32 done, u32 total))
{
	struct extlist x;
	int i;

	x.n = 0;
	for (i = 0; i < 3 && f->ext[i][1]; i++) {
		x.start[x.n] = f->ext[i][0];
		x.count[x.n] = f->ext[i][1];
		x.n++;
	}
	if (ext_blocks(&x) * blk_size < f->size)
		ext_overflow(f->id, &x);
	if (off + len > f->size)
		return -1;
	return ext_read(&x, off, len, buf, progress);
}
