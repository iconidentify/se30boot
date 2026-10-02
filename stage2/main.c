/* main.c - se30boot's menu: pick a kernel, edit its command line, boot.
 *
 * The configuration is a text file on the boot volume, normally
 * Linux:se30boot.cfg:
 *
 *	timeout 5		seconds before the default boots; -1 waits
 *	default 1		menu number, or a title
 *	title Linux		starts an entry
 *	kernel Linux:vmlinux.gz	a path on the boot volume
 *	append root=/dev/sda3	the kernel's command line
 *	title Mac OS
 *	macos			continue booting Mac OS
 *
 * and, for when Mac OS can't show all the RAM (24-bit mode), any number of
 *	memory ADDRESS SIZE	physical RAM to tell kernels about instead
 */
#include "boot.h"



static struct entry entries[MAXENTRIES];
static int nentries, deflt, timeout = 5;
static char volname[28];
static const char *cfgname;
static int top_row;			/* first menu row */

/* --- configuration --- */

static const char *const cfg_paths[] = {
	"Linux:se30boot.cfg", "se30boot.cfg", NULL
};

static void copy_value(char *dst, int size, const char *s, const char *end)
{
	int n;

	while (s < end && (*s == ' ' || *s == '\t'))
		s++;
	while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	n = end - s;
	if (n > size - 1)
		n = size - 1;
	memcpy(dst, s, n);
	dst[n] = 0;
}

static int keyword(const char *s, const char *end, const char *word, const char **rest)
{
	int n = strlen(word);

	if (end - s < n || strncasecmp_(s, word, n) ||
	    (end - s > n && s[n] != ' ' && s[n] != '\t'))
		return 0;
	*rest = s + n;
	return 1;
}

/* 123, 0x7b, 64M or 512K */
static u32 number(const char **p)
{
	const char *s = *p;
	u32 v = 0;

	while (*s == ' ' || *s == '\t')
		s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		for (s += 2;; s++) {
			int c = *s | 32;

			if (c >= '0' && c <= '9')
				v = v * 16 + c - '0';
			else if (c >= 'a' && c <= 'f')
				v = v * 16 + c - 'a' + 10;
			else
				break;
		}
	} else {
		while (*s >= '0' && *s <= '9')
			v = v * 10 + *s++ - '0';
	}
	if ((*s | 32) == 'k')
		v <<= 10, s++;
	else if ((*s | 32) == 'm')
		v <<= 20, s++;
	*p = s;
	return v;
}

static void parse_config(const char *text, u32 len)
{
	const char *p = text, *end = text + len;
	struct entry *e = NULL;
	char defname[64] = "";
	int i;

	while (p < end) {
		const char *line = p, *eol, *rest;

		while (p < end && *p != '\r' && *p != '\n')
			p++;
		eol = p;
		while (p < end && (*p == '\r' || *p == '\n'))
			p++;
		while (line < eol && (*line == ' ' || *line == '\t'))
			line++;
		if (line == eol || *line == '#')
			continue;
		if (keyword(line, eol, "title", &rest)) {
			if (nentries == MAXENTRIES)
				break;
			e = &entries[nentries++];
			memset(e, 0, sizeof(*e));
			copy_value(e->title, sizeof(e->title), rest, eol);
		} else if (keyword(line, eol, "timeout", &rest)) {
			char v[12];
			int neg = 0, n = 0;
			const char *q = v;

			copy_value(v, sizeof(v), rest, eol);
			if (*q == '-')
				neg = 1, q++;
			while (*q >= '0' && *q <= '9')
				n = n * 10 + *q++ - '0';
			timeout = neg ? -1 : n;
		} else if (keyword(line, eol, "memory", &rest)) {
			if (nmem_override < MAXCHUNKS) {
				mem_override[nmem_override].addr = number(&rest);
				mem_override[nmem_override].size = number(&rest);
				if (mem_override[nmem_override].size)
					nmem_override++;
			}
		} else if (keyword(line, eol, "debug", &rest)) {
			debug = 1;
		} else if (keyword(line, eol, "default", &rest)) {
			copy_value(defname, sizeof(defname), rest, eol);
		} else if (e && keyword(line, eol, "kernel", &rest)) {
			copy_value(e->kernel, sizeof(e->kernel), rest, eol);
		} else if (e && keyword(line, eol, "append", &rest)) {
			copy_value(e->append, sizeof(e->append), rest, eol);
		} else if (e && keyword(line, eol, "macos", &rest)) {
			e->macos = 1;
		}
	}
	deflt = 0;
	if (defname[0] >= '1' && defname[0] <= '9' && !defname[1])
		deflt = defname[0] - '1';
	else
		for (i = 0; i < nentries; i++)
			if (!strncasecmp_(entries[i].title, defname, -1))
				deflt = i;
	if (deflt >= nentries)
		deflt = 0;
}

static void load_config(void)
{
	struct hfs_file f;
	int i;

	for (i = 0; cfg_paths[i]; i++) {
		u8 *buf;

		if (hfs_lookup(cfg_paths[i], &f) || f.size > 16384)
			continue;
		buf = heap_alloc(f.size + 1);
		if (!buf || hfs_read(&f, 0, f.size, buf, NULL))
			continue;
		cfgname = cfg_paths[i];
		parse_config((const char *)buf, f.size);
		break;
	}
	if (!nentries) {			/* no file: Mac OS only */
		strcpy(entries[0].title, "Mac OS");
		entries[0].macos = 1;
		nentries = 1;
		timeout = -1;
	}
}

/* --- keyboard --- */

/* Mac OS's event queue isn't set up when the boot blocks run, but the
   ROM's keyboard driver keeps KeyMap, a bit per key, as it does for the
   startup key combinations. Characters come from these US tables by
   virtual key code; arrows are 0x7b-0x7e. */
#define KEY_UP		0x1e
#define KEY_DOWN	0x1f
#define KEY_LEFT	0x1c
#define KEY_RIGHT	0x1d
#define KEY_ESC		0x1b
#define KEY_BS		0x08

static const char plain[128] =
	"asdfhgzxcv\0bqweryt123465=97-80]ou[ip\rlj'k;\\,/nm.\t `\b\r\x1b";
static const char shifted[128] =
	"ASDFHGZXCV\0BQWERYT!@#$^%+(&_*)}OU{IP\rLJ\"K:|<?NM>\t ~\b\r\x1b";

static int key_char(int k, const u8 *map)
{
	int shift = (map[7] & 1) || (map[7] & 0x10);	/* 0x38, 0x3c */

	switch (k) {
	case 0x7b: return KEY_LEFT;
	case 0x7c: return KEY_RIGHT;
	case 0x7d: return KEY_DOWN;
	case 0x7e: return KEY_UP;
	case 0x4c: return '\r';				/* Enter */
	case 0x47: return KEY_ESC;			/* Clear */
	case 0x75: return KEY_BS;			/* Del */
	}
	if (k >= 0x52 && k <= 0x5c && k != 0x5a)	/* keypad digits */
		return "01234567x89"[k - 0x52];
	if (k < 0x36)
		return (shift ? shifted : plain)[k] ? (shift ? shifted : plain)[k] : -1;
	return -1;
}

static u8 prev_map[16];
static int held = -1;
static u32 held_since, last_repeat;

/* a character, or -1 if none; keys repeat after half a second */
static int key(void)
{
	u8 map[16];
	int k, c = -1;

	memcpy(map, (const void *)KeyMap, 16);
	for (k = 0; k < 128 && c < 0; k++) {
		int bit = 1 << (k & 7);

		if ((map[k >> 3] & bit) && !(prev_map[k >> 3] & bit)) {
			c = key_char(k, map);
			if (c >= 0) {
				held = k;
				held_since = last_repeat = Ticks;
			}
		}
	}
	if (held >= 0 && !(map[held >> 3] & (1 << (held & 7))))
		held = -1;
	if (c < 0 && held >= 0 && Ticks - held_since > 30 && Ticks - last_repeat >= 4) {
		last_repeat = Ticks;
		c = key_char(held, map);
	}
	memcpy(prev_map, map, 16);
	return c;
}

int wait_key(void)
{
	int c;

	while ((c = key()) < 0)
		;
	return c;
}

/* --- screen --- */

static int msg_row, bar_row, info_row;

void message(const char *fmt, ...)
{
	char buf[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf_(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	con_fill_row(msg_row, 0);
	con_text(2, msg_row, buf, 0);
}

void progress_bar(u32 done, u32 total)
{
	con_bar(bar_row, done, total);
}

static void draw_menu(int sel)
{
	int i;

	for (i = 0; i < nentries; i++) {
		con_fill_row(top_row + i, i == sel);
		con_printf(2, top_row + i, i == sel, "%d  %s", i + 1, entries[i].title);
	}
}

static void draw_screen(int sel)
{
	char mem[96];
	int i, n = 0;
	u32 total = 0;

	con_clear();
	con_fill_row(0, 1);
	con_printf(1, 0, 1, "se30boot  -  %s", volname);
	top_row = 2;
	draw_menu(sel);
	con_text(2, top_row + nentries + 1,
		 "Up/Down or 1-9 choose, Return boots, E edits the command line.", 0);
	msg_row = con_rows - 5;
	bar_row = con_rows - 4;
	info_row = con_rows - 2;
	if (nmem_override) {
		for (i = 0; i < nmem_override; i++)
			total += mem_override[i].size;
		n = snprintf_(mem, sizeof(mem), "%u MB RAM (from the config):", total >> 20);
		for (i = 0; i < nmem_override && n < (int)sizeof(mem) - 24; i++)
			n += snprintf_(mem + n, sizeof(mem) - n, " %u MB at %p%s",
				       mem_override[i].size >> 20, mem_override[i].addr,
				       i < nmem_override - 1 ? "," : "");
	} else {
		snprintf_(mem, sizeof(mem), "%u MB RAM, %s addressing", MemTop >> 20,
			  LM8(0xcb2) ? "32-bit" : "24-bit");
	}
	con_text(2, info_row, mem, 0);
	con_printf(2, info_row + 1, 0, "Config %s", cfgname ? cfgname : "not found");
	if (!LM8(0xcb2) && !nmem_override)
		con_text(2, info_row - 1,
			 "Kernels only see 8 MB: turn on 32-bit addressing (Memory control panel).", 0);
}

/* edit the command line on the message rows; 0 if cancelled */
static int edit(struct entry *e)
{
	char line[256];
	int len, c, row = msg_row - 2, width = con_cols - 4;

	strcpy(line, e->append);
	len = strlen(line);
	con_fill_row(row - 1, 0);
	con_printf(2, row - 1, 0, "Command line for %s (Return boots, Esc cancels):",
		   e->title);
	for (;;) {
		int start = len > width - 1 ? len - (width - 1) : 0;

		con_fill_row(row, 0);
		con_text(2, row, line + start, 0);
		con_text(2 + len - start, row, " ", 1);	/* the cursor */
		c = wait_key();
		if (c == '\r') {
			strcpy(e->append, line);
			return 1;
		}
		if (c == KEY_ESC)
			return 0;
		if (c == KEY_BS) {
			if (len)
				line[--len] = 0;
		} else if (c >= ' ' && c < 0x7f && len < (int)sizeof(line) - 1) {
			line[len++] = c;
			line[len] = 0;
		}
	}
}

static void boot(struct entry *e)
{
	const char *err;

	if (e->macos) {
		con_gray();
		chain_macos();
	}
	err = load_kernel(e);
	message("Can't boot %s: %s", e->title, err);
	con_fill_row(msg_row + 1, 0);
	con_text(2, msg_row + 1, "Press a key.", 0);
	wait_key();
}

int main(void)
{
	int sel, c, i, countdown;
	u32 start, shown = 0xffffffff;

	con_init((u8 *)ScrnBase, ScreenRow, 512, 342);
	if (CrsrPin[3] - CrsrPin[1] > 0 && CrsrPin[2] - CrsrPin[0] > 0 &&
	    CrsrPin[3] - CrsrPin[1] <= ScreenRow * 8)
		con_init((u8 *)ScrnBase, ScreenRow, CrsrPin[3] - CrsrPin[1],
			 CrsrPin[2] - CrsrPin[0]);
	con_clear();
	con_text(2, 1, "se30boot", 0);
	heap_init();
	i = hfs_mount();
	if (i) {
		con_printf(2, 3, 0, "Can't read the boot volume (%d, error %d).", i, hfs_error);
		con_text(2, 4, "Press a key to start Mac OS.", 0);
		wait_key();
		con_gray();
		chain_macos();
	}
	hfs_volname(volname);
	load_config();

	sel = deflt;
	countdown = timeout;
	for (;;) {
		draw_screen(sel);
		start = Ticks;
		for (;;) {
			if (countdown >= 0) {
				u32 left = countdown - (Ticks - start) / 60;

				if ((s32)left <= 0 || (Ticks - start) / 60 >= (u32)countdown) {
					countdown = -1;
					boot(&entries[sel]);
					break;
				}
				if (left != shown) {
					message("%s starts in %u s.", entries[sel].title, left);
					shown = left;
				}
			}
			c = key();
			if (c < 0)
				continue;
			if (countdown >= 0) {
				countdown = -1;
				message("");
			}
			if (c == KEY_UP && sel > 0)
				draw_menu(--sel);
			else if (c == KEY_DOWN && sel < nentries - 1)
				draw_menu(++sel);
			else if (c >= '1' && c < '1' + nentries)
				draw_menu(sel = c - '1');
			else if (c == '\r') {
				boot(&entries[sel]);
				break;
			} else if ((c == 'e' || c == 'E') && !entries[sel].macos) {
				if (edit(&entries[sel])) {
					boot(&entries[sel]);
				}
				break;
			}
		}
	}
}
