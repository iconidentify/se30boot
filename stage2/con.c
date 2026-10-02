/* con.c - text straight onto the Mac's 1-bit screen, 6x13 characters. */
#include "boot.h"

extern const unsigned char font6x13[256][13];

#define CW 6
#define CH 13

u8 *con_base;
int con_rowbytes, con_cols, con_rows;
static int con_width, con_height, con_top;

void con_init(u8 *base, int rowbytes, int width, int height)
{
	con_base = base;
	con_rowbytes = rowbytes;
	con_width = width;
	con_height = height;
	con_cols = width / CW;
	con_rows = height / CH;
	con_top = (height - con_rows * CH) / 2;
}

void con_clear(void)
{
	int y;

	for (y = 0; y < con_height; y++)
		memset(con_base + y * con_rowbytes, 0, con_width / 8);
}

/* Mac OS's gray desktop, as the screen was before the menu */
void con_gray(void)
{
	int y;

	for (y = 0; y < con_height; y++)
		memset(con_base + y * con_rowbytes, y & 1 ? 0x55 : 0xaa, con_width / 8);
}

/* a row of text cells, white on black when inverse */
void con_fill_row(int row, int inverse)
{
	int y;
	u8 *p = con_base + (con_top + row * CH) * con_rowbytes;

	for (y = 0; y < CH; y++, p += con_rowbytes)
		memset(p, inverse ? 0xff : 0, con_width / 8);
}

static void glyph(int col, int row, u8 c, int inverse)
{
	int x = col * CW, shift = 10 - (x & 7), y;
	u8 *p = con_base + (con_top + row * CH) * con_rowbytes + (x >> 3);
	u16 mask = ~(0xfc00 >> (x & 7));

	for (y = 0; y < CH; y++, p += con_rowbytes) {
		u16 bits = (font6x13[c][y] >> 2) << shift;
		u16 old = p[0] << 8 | p[1];

		if (inverse)
			bits ^= 0x3f << shift;
		old = (old & mask) | bits;
		p[0] = old >> 8;
		if (x + CW > ((x >> 3) + 1) * 8)
			p[1] = old;
	}
}

void con_text(int col, int row, const char *s, int inverse)
{
	if (row < 0 || row >= con_rows)
		return;
	for (; *s && col < con_cols; s++, col++)
		if (col >= 0)
			glyph(col, row, *s, inverse);
}

void con_printf(int col, int row, int inverse, const char *fmt, ...)
{
	char buf[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf_(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	con_text(col, row, buf, inverse);
}

/* a progress bar across a text row */
void con_bar(int row, u32 done, u32 total)
{
	int w = con_cols * CW - 2 * CW, fill, y;
	u8 *p = con_base + (con_top + row * CH + 3) * con_rowbytes;

	if (!total)
		return;
	if (done > total)
		done = total;
	fill = (u32)((unsigned long long)done * w / total);
	for (y = 0; y < CH - 6; y++, p += con_rowbytes) {
		int x;

		for (x = 0; x < w; x++) {
			int px = CW + x;
			u8 bit = 0x80 >> (px & 7);
			int on = x < fill || y == 0 || y == CH - 7 || x == 0 || x == w - 1;

			if (on)
				p[px >> 3] |= bit;
			else
				p[px >> 3] &= ~bit;
		}
	}
}
