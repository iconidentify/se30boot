/* inflate.c - gzip decompression (RFC 1951 and 1952), as a stream.
 *
 * Input comes in pieces from a callback (the disk), and output goes through
 * a 32 KB window to another callback (which places the kernel's segments),
 * so the compressed file and the unpacked ELF file never have to be in
 * memory whole. Codes up to FAST bits long decode with one table lookup;
 * longer ones walk the canonical code a bit at a time, as in zlib's puff. */
#include "boot.h"

#define FAST	10
#define MAXBITS	15

struct huff {
	u16 fast[1 << FAST];		/* symbol << 4 | length, or 0 */
	u16 count[MAXBITS + 1];		/* how many codes of each length */
	u16 sym[288];			/* symbols in canonical code order */
};

#define WSIZE	0x8000
#define WMASK	(WSIZE - 1)

struct state {
	const u8 *in, *end;
	u32 bitbuf;
	int bitcnt, overrun;
	u32 total;			/* bytes out so far */
	int (*fill)(const u8 **buf, u32 *len);
	int (*out)(u32 off, const u8 *data, u32 len);
	int failed;			/* the out callback refused */
};

static u8 window[WSIZE];

static struct huff lit, dist;

static const u16 len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
	3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const u16 dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
	8193, 12289, 16385, 24577 };
static const u8 dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
	7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

enum { E_OK, E_MAGIC = -1, E_METHOD = -2, E_INPUT = -3, E_OUTPUT = -4,
       E_BLOCK = -5, E_CODES = -6, E_DIST = -7, E_SIZE = -8 };

const char *gunzip_error(int e)
{
	switch (e) {
	case E_MAGIC: return "not a gzip file";
	case E_METHOD: return "unknown compression method";
	case E_INPUT: return "file is truncated";
	case E_OUTPUT: return "not enough memory to unpack";
	case E_BLOCK: return "bad block type";
	case E_CODES: return "bad Huffman codes";
	case E_DIST: return "distance too far back";
	case E_SIZE: return "unpacked size is wrong";
	}
	return "corrupt data";
}

static void more(struct state *s)
{
	const u8 *buf;
	u32 len;

	if (s->fill(&buf, &len) == 0 && len) {
		s->in = buf;
		s->end = buf + len;
	}
}

static inline void refill(struct state *s)
{
	while (s->bitcnt <= 24) {
		if (s->in == s->end)
			more(s);
		if (s->in < s->end)
			s->bitbuf |= (u32)*s->in++ << s->bitcnt;
		else
			s->overrun++;
		s->bitcnt += 8;
	}
}

/* hand the window's newest bytes to the out callback */
static void flush(struct state *s)
{
	u32 n = s->total & WMASK ? s->total & WMASK : WSIZE;

	if (!s->failed && s->out(s->total - n, window, n))
		s->failed = 1;
}

static inline void put(struct state *s, u8 c)
{
	window[s->total++ & WMASK] = c;
	if (!(s->total & WMASK))
		flush(s);
}

static inline u32 bits(struct state *s, int n)
{
	u32 v;

	refill(s);
	v = s->bitbuf & ((1u << n) - 1);
	s->bitbuf >>= n;
	s->bitcnt -= n;
	return v;
}

static int build(struct huff *h, const u8 *lens, int n)
{
	u16 offs[MAXBITS + 2];
	int i, len, left, k;
	u32 code;

	memset(h->count, 0, sizeof(h->count));
	for (i = 0; i < n; i++)
		h->count[lens[i]]++;
	left = 1;
	for (len = 1; len <= MAXBITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return -1;	/* over-subscribed */
	}
	offs[1] = 0;
	for (len = 1; len < MAXBITS; len++)
		offs[len + 1] = offs[len] + h->count[len];
	for (i = 0; i < n; i++)
		if (lens[i])
			h->sym[offs[lens[i]]++] = i;

	memset(h->fast, 0, sizeof(h->fast));
	code = 0;
	k = 0;
	for (len = 1; len <= MAXBITS; len++) {
		int j;

		for (j = 0; j < h->count[len]; j++, k++, code++) {
			u32 rev = 0, c = code, x;
			int b;

			if (len > FAST)
				continue;
			for (b = 0; b < len; b++) {	/* deflate sends codes backwards */
				rev = rev << 1 | (c & 1);
				c >>= 1;
			}
			for (x = rev; x < (1u << FAST); x += 1u << len)
				h->fast[x] = h->sym[k] << 4 | len;
		}
		code <<= 1;
	}
	return left;
}

static inline int decode(struct state *s, const struct huff *h)
{
	u16 e;
	int code, first, index, len;

	refill(s);
	e = h->fast[s->bitbuf & ((1 << FAST) - 1)];
	if (e) {
		s->bitbuf >>= e & 15;
		s->bitcnt -= e & 15;
		return e >> 4;
	}
	code = first = index = 0;
	for (len = 1; len <= MAXBITS; len++) {
		int count = h->count[len];

		code |= s->bitbuf & 1;
		s->bitbuf >>= 1;
		s->bitcnt--;
		if (code - count < first)
			return h->sym[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

/* The hot loop: one block's literals and matches. The bit buffer, input
   and window pointers live in locals so they stay in registers (stores to
   the window could alias the state struct otherwise), and the tables are
   reached through address registers. */
static int codes(struct state *s)
{
	u32 bitbuf = s->bitbuf;
	int bitcnt = s->bitcnt;
	const u8 *in = s->in, *end = s->end;
	u32 flushed = s->total & ~WMASK;	/* output before this window */
	u8 *win = window, *wend, *op;
	const u16 *lfast = lit.fast, *dfast = dist.fast;

	__asm__("" : "+a"(win), "+a"(lfast), "+a"(dfast));
	wend = win + WSIZE;
	op = win + (s->total & WMASK);

#define SAVE()	(s->bitbuf = bitbuf, s->bitcnt = bitcnt, s->in = in, s->end = end, \
		 s->total = flushed + (u32)(op - win))
#define LOAD()	(bitbuf = s->bitbuf, bitcnt = s->bitcnt, in = s->in, end = s->end)
#define REFILL() \
	if (bitcnt <= 24) { \
		if (end - in >= 4) { \
			do { \
				bitbuf |= (u32)*in++ << bitcnt; \
				bitcnt += 8; \
			} while (bitcnt <= 24); \
		} else { \
			SAVE(); \
			refill(s); \
			LOAD(); \
			if (s->overrun > 4) \
				return E_INPUT; \
		} \
	}
#define BITS(n, v) do { \
		REFILL(); \
		v = bitbuf & ((1u << (n)) - 1); \
		bitbuf >>= (n); \
		bitcnt -= (n); \
	} while (0)
#define DECODE(fast, h, sym) do { \
		u16 e_; \
		REFILL(); \
		e_ = fast[bitbuf & ((1 << FAST) - 1)]; \
		if (e_) { \
			bitbuf >>= e_ & 15; \
			bitcnt -= e_ & 15; \
			sym = e_ >> 4; \
		} else { \
			SAVE(); \
			sym = decode(s, h); \
			LOAD(); \
		} \
	} while (0)
#define FLUSH() do { \
		s->total = flushed + WSIZE; \
		flush(s); \
		if (s->failed) \
			return E_OUTPUT; \
		flushed += WSIZE; \
		op = win; \
	} while (0)

	for (;;) {
		int sym;

		DECODE(lfast, &lit, sym);
		if (sym < 256) {
			if (sym < 0)
				return E_CODES;
			*op++ = sym;
			if (op == wend)
				FLUSH();
		} else if (sym == 256) {
			break;
		} else {
			u32 len, d, extra;
			const u8 *sp;

			sym -= 257;
			if (sym >= 29)
				return E_CODES;
			len = len_base[sym];
			if (len_extra[sym]) {
				BITS(len_extra[sym], extra);
				len += extra;
			}
			DECODE(dfast, &dist, sym);
			if (sym < 0 || sym >= 30)
				return E_CODES;
			d = dist_base[sym];
			if (dist_extra[sym]) {
				BITS(dist_extra[sym], extra);
				d += extra;
			}
			if (d > flushed + (u32)(op - win))
				return E_DIST;
			sp = op - d;
			if (sp < win)
				sp += WSIZE;
			if (op + len <= wend && sp + len <= wend) {
				/* no wrap: a plain byte copy (overlap repeats bytes,
				   as deflate means it to) */
				do
					*op++ = *sp++;
				while (--len);
				if (op == wend)
					FLUSH();
			} else {
				while (len--) {
					*op++ = *sp++;
					if (sp == wend)
						sp = win;
					if (op == wend)
						FLUSH();
				}
			}
		}
	}
	SAVE();
	return E_OK;
#undef SAVE
#undef LOAD
#undef REFILL
#undef BITS
#undef DECODE
#undef FLUSH
}

static int stored(struct state *s)
{
	u32 len, nlen;

	s->bitbuf >>= s->bitcnt & 7;	/* to a byte boundary */
	s->bitcnt &= ~7;
	len = bits(s, 16);
	nlen = bits(s, 16);
	if ((len ^ 0xffff) != nlen)
		return E_BLOCK;
	while (len--)
		put(s, bits(s, 8));
	return s->overrun > 4 ? E_INPUT : s->failed ? E_OUTPUT : E_OK;
}

static int fixed(struct state *s)
{
	static int built;
	static struct huff flit, fdist;
	u8 lens[288];
	int i;

	if (!built) {
		for (i = 0; i < 144; i++) lens[i] = 8;
		for (; i < 256; i++) lens[i] = 9;
		for (; i < 280; i++) lens[i] = 7;
		for (; i < 288; i++) lens[i] = 8;
		build(&flit, lens, 288);
		for (i = 0; i < 30; i++) lens[i] = 5;
		build(&fdist, lens, 30);
		built = 1;
	}
	lit = flit;
	dist = fdist;
	return codes(s);
}

static int dynamic(struct state *s)
{
	static const u8 order[19] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	u8 lens[320];
	int nlen, ndist, ncode, i;

	nlen = bits(s, 5) + 257;
	ndist = bits(s, 5) + 1;
	ncode = bits(s, 4) + 4;
	if (nlen > 286 || ndist > 30)
		return E_CODES;
	memset(lens, 0, 19);
	for (i = 0; i < ncode; i++)
		lens[order[i]] = bits(s, 3);
	if (build(&lit, lens, 19) != 0)
		return E_CODES;
	for (i = 0; i < nlen + ndist;) {
		int sym = decode(s, &lit), len = 0, rep;

		if (sym < 0)
			return E_CODES;
		if (sym < 16) {
			lens[i++] = sym;
			continue;
		}
		if (sym == 16) {
			if (!i)
				return E_CODES;
			len = lens[i - 1];
			rep = 3 + bits(s, 2);
		} else if (sym == 17) {
			rep = 3 + bits(s, 3);
		} else {
			rep = 11 + bits(s, 7);
		}
		if (i + rep > nlen + ndist)
			return E_CODES;
		while (rep--)
			lens[i++] = len;
	}
	if (!lens[256])
		return E_CODES;
	i = build(&lit, lens, nlen);
	if (i < 0 || (i > 0 && nlen != lit.count[0] + lit.count[1]))
		return E_CODES;
	i = build(&dist, lens + nlen, ndist);
	if (i < 0 || (i > 0 && ndist != dist.count[0] + dist.count[1]))
		return E_CODES;
	return codes(s);
}

/* Unpack a gzip stream from fill to out. The first piece fill gives must
   hold the whole gzip header. Returns 0 or an E_ code; *outlen is the
   unpacked size. */
int gunzip(int (*fill)(const u8 **buf, u32 *len),
	   int (*out)(u32 off, const u8 *data, u32 len), u32 *outlen)
{
	struct state s;
	const u8 *p, *end;
	u32 len, size;
	int flags, last, err, i;

	memset(&s, 0, sizeof(s));
	s.fill = fill;
	s.out = out;
	if (fill(&p, &len) || len < 18)
		return E_INPUT;
	end = p + len;
	if (p[0] != 0x1f || p[1] != 0x8b)
		return E_MAGIC;
	if (p[2] != 8)
		return E_METHOD;
	flags = p[3];
	p += 10;
	if (flags & 4)				/* FEXTRA */
		p += 2 + (p[0] | p[1] << 8);
	if (flags & 8)				/* FNAME */
		while (p < end && *p++)
			;
	if (flags & 16)				/* FCOMMENT */
		while (p < end && *p++)
			;
	if (flags & 2)				/* FHCRC */
		p += 2;
	if (p >= end)
		return E_INPUT;
	s.in = p;
	s.end = end;
	do {
		last = bits(&s, 1);
		switch (bits(&s, 2)) {
		case 0: err = stored(&s); break;
		case 1: err = fixed(&s); break;
		case 2: err = dynamic(&s); break;
		default: err = E_BLOCK;
		}
		if (err)
			return err;
	} while (!last);
	if (s.total & WMASK)
		flush(&s);
	if (s.failed)
		return E_OUTPUT;
	/* the trailer: CRC-32 (not checked), then the size mod 2^32 */
	s.bitbuf >>= s.bitcnt & 7;
	s.bitcnt &= ~7;
	bits(&s, 16);
	bits(&s, 16);
	size = 0;
	for (i = 0; i < 4; i++)
		size |= bits(&s, 8) << (8 * i);
	if (s.overrun > 4)
		return E_INPUT;
	*outlen = s.total;
	return size == s.total ? E_OK : E_SIZE;
}
