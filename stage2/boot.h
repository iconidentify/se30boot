#ifndef BOOT_H
#define BOOT_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef short s16;
typedef int s32;

#define NULL ((void *)0)

#include <stdarg.h>

/* Memory while Mac OS is still running. Stage 1 reads us to STAGE2 and
   saves the ROM's registers at SAVE_AREA, so both stay out of the heap. */
#define STAGE2		0x00700000
#define STAGE2_TOP	0x00780000
#define SAVE_AREA	0x006fff00

/* Mac OS low-memory globals */
#define LM8(a)		(*(volatile u8 *)(a))
#define LM16(a)		(*(volatile u16 *)(a))
#define LM32(a)		(*(volatile u32 *)(a))
#define ScreenRow	LM16(0x106)
#define MemTop		LM32(0x108)
#define BufPtr		LM32(0x10c)
#define HeapEnd		LM32(0x114)
#define ApplLimit	LM32(0x130)
#define Ticks		LM32(0x16a)
#define KeyMap		((volatile u8 *)0x174)
#define SCCRd		LM32(0x1d8)
#define Time		LM32(0x20c)
#define SysZone		LM32(0x2a6)
#define ApplZone	LM32(0x2aa)
#define ROMBase		LM32(0x2ae)
#define ScrnBase	LM32(0x824)
#define CrsrPin		((volatile s16 *)0x834)

/* start.S */
struct mmu_regs {
	u32 tc, crp[2], srp[2], tt0, tt1;
};
int rom_read(void *pb);
int rom_osevent(int mask, void *event);
void rom_readxpram(void *buf, int addr, int count);
void chain_macos(void) __attribute__((noreturn));
#define MOVER	0x800			/* the final copy runs here */
void go_kernel(const void *pieces, int n, u32 dst, u32 entry) __attribute__((noreturn));
u32 get_sp(void);
int fpu_probe(void);
extern u32 bootblock;

/* lib.c */
void *memcpy(void *d, const void *s, unsigned long n);
void *memmove(void *d, const void *s, unsigned long n);
void *memset(void *d, int c, unsigned long n);
int strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncasecmp_(const char *a, const char *b, int n);
char *strcpy(char *d, const char *s);
int vsnprintf_(char *buf, int size, const char *fmt, va_list ap);
int snprintf_(char *buf, int size, const char *fmt, ...);

/* con.c: text on the screen */
extern u8 *con_base;
extern int con_rowbytes, con_cols, con_rows;
void con_init(u8 *base, int rowbytes, int width, int height);
void con_clear(void);
void con_gray(void);
void con_text(int col, int row, const char *s, int inverse);
void con_fill_row(int row, int inverse);
void con_printf(int col, int row, int inverse, const char *fmt, ...);
void con_bar(int row, u32 done, u32 total);

/* hfs.c */
struct hfs_file {
	u32 id, size;
	u16 ext[3][2];			/* the first data extents: start, count */
};
int hfs_mount(void);
void hfs_volname(char *buf);
int hfs_lookup(const char *path, struct hfs_file *f);
int hfs_read(const struct hfs_file *f, u32 off, u32 len, void *buf,
	     void (*progress)(u32 done, u32 total));
extern int hfs_error;

/* inflate.c */
int gunzip(int (*fill)(const u8 **buf, u32 *len),
	   int (*out)(u32 off, const u8 *data, u32 len), u32 *outlen);
const char *gunzip_error(int e);

/* mmu.c: Mac OS's logical memory, and the physical RAM behind it */
#define MAXBANKS 32
#define MAXCHUNKS 8
struct bank { u32 log, phys, size; };	/* contiguous both ways */
struct chunk { u32 addr, size; };	/* physical RAM */
extern struct mmu_regs mmu;
extern struct bank bank[MAXBANKS];
extern int nbanks;
extern struct chunk chunk[MAXCHUNKS];
extern int nchunks;
int mmu_scan(void);
u32 mem_top(void);
int log2phys(u32 log, u32 *phys);
u32 contiguous_at(u32 log);
u32 bank_phys(u32 log);

/* load.c */
struct entry {
	char title[64];
	char kernel[128];
	char append[256];
	int macos;
};
void heap_init(void);
u8 *heap_alloc(u32 size);
u32 heap_free(void);
const char *load_kernel(const struct entry *e);	/* returns only on error */
extern struct chunk mem_override[MAXCHUNKS];
extern int debug;
extern int fpu_type;
void fpu_detect(void);
extern int nmem_override;
extern const char *status;

/* main.c */
#define MAXENTRIES 9
void message(const char *fmt, ...);
int wait_key(void);
void progress_bar(u32 done, u32 total);

#endif
