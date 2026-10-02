/* lib.c - the bits of a C library stage 2 needs. */
#include "boot.h"

void *memcpy(void *d, const void *s, unsigned long n)
{
	u8 *dp = d;
	const u8 *sp = s;

	if (!(((u32)dp | (u32)sp) & 3)) {
		while (n >= 16) {
			((u32 *)dp)[0] = ((const u32 *)sp)[0];
			((u32 *)dp)[1] = ((const u32 *)sp)[1];
			((u32 *)dp)[2] = ((const u32 *)sp)[2];
			((u32 *)dp)[3] = ((const u32 *)sp)[3];
			dp += 16;
			sp += 16;
			n -= 16;
		}
	}
	while (n--)
		*dp++ = *sp++;
	return d;
}

void *memmove(void *d, const void *s, unsigned long n)
{
	u8 *dp = d;
	const u8 *sp = s;

	if (dp <= sp || dp >= sp + n)
		return memcpy(d, s, n);
	while (n--)
		dp[n] = sp[n];
	return d;
}

void *memset(void *d, int c, unsigned long n)
{
	u8 *dp = d;

	if (!((u32)dp & 3) && n >= 4) {
		u32 v = (u8)c * 0x01010101u;

		while (n >= 4) {
			*(u32 *)dp = v;
			dp += 4;
			n -= 4;
		}
	}
	while (n--)
		*dp++ = c;
	return d;
}

int strlen(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (u8)*a - (u8)*b;
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* compare n bytes ignoring ASCII case; n < 0 means up to the NULs */
int strncasecmp_(const char *a, const char *b, int n)
{
	while (n--) {
		int d = lower((u8)*a) - lower((u8)*b);

		if (d || !*a)
			return d;
		a++, b++;
	}
	return 0;
}

char *strcpy(char *d, const char *s)
{
	char *r = d;

	while ((*d++ = *s++))
		;
	return r;
}

/* printf: %s %c %d %u %x %p, with '-', '0' and a width; 'l' is ignored */
struct sbuf {
	char *p;
	int left, n;
};

static void put(struct sbuf *b, char c)
{
	if (b->left > 1) {
		*b->p++ = c;
		b->left--;
	}
	b->n++;
}

static void num(struct sbuf *b, u32 v, int base, int neg, int width, char pad,
		int left)
{
	char t[12];
	int n = 0, len;

	do {
		t[n++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	if (neg)
		t[n++] = '-';
	len = n;
	while (!left && width-- > len)
		put(b, pad);
	while (n--)
		put(b, t[n]);
	while (left && width-- > len)
		put(b, ' ');
}

int vsnprintf_(char *buf, int size, const char *fmt, va_list ap)
{
	struct sbuf b = { buf, size, 0 };

	for (; *fmt; fmt++) {
		int width = 0, left = 0;
		char pad = ' ';

		if (*fmt != '%') {
			put(&b, *fmt);
			continue;
		}
		fmt++;
		if (*fmt == '-')
			left = 1, fmt++;
		if (*fmt == '0')
			pad = '0', fmt++;
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + *fmt++ - '0';
		while (*fmt == 'l')
			fmt++;
		switch (*fmt) {
		case 's': {
			const char *s = va_arg(ap, const char *);
			int len = strlen(s);

			while (!left && width-- > len)
				put(&b, ' ');
			while (*s)
				put(&b, *s++);
			while (left && width-- > len)
				put(&b, ' ');
			break;
		}
		case 'c':
			put(&b, va_arg(ap, int));
			break;
		case 'd': {
			int v = va_arg(ap, int);

			num(&b, v < 0 ? -v : v, 10, v < 0, width, pad, left);
			break;
		}
		case 'u':
			num(&b, va_arg(ap, u32), 10, 0, width, pad, left);
			break;
		case 'x':
			num(&b, va_arg(ap, u32), 16, 0, width, pad, left);
			break;
		case 'p':
			put(&b, '0');
			put(&b, 'x');
			num(&b, va_arg(ap, u32), 16, 0, 8, '0', 0);
			break;
		case 0:
			fmt--;
			break;
		default:
			put(&b, *fmt);
		}
	}
	if (size > 0)
		*b.p = 0;
	return b.n;
}

int snprintf_(char *buf, int size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf_(buf, size, fmt, ap);
	va_end(ap);
	return n;
}
