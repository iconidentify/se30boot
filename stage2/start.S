| start.S - stage 2's entry, and what C can't do: ROM traps, reading
| physical memory under Mac OS's MMU, and leaving Mac OS for a kernel.

	.equ	SAVE_AREA,	0x006fff00	| stage 1 saved the ROM's registers here
	.equ	LOWFN,		0x100		| phys_read's helper, in low memory
	.equ	LOWTT,		0x138		| its transparent translation value
	.equ	LOWOLD,		0x13c		| and the TT0 to put back
	.equ	MOVER,		0x800		| the final copy runs here
	.equ	MOVER_SCRATCH,	0x7f0
	.equ	PIECES,		0x700		| the kernel image's pieces, for it

	.section .text.head,"ax"
	.globl	_start
_start:
	bra.s	1f
	.ascii	"SE3B"			| stage 1 checks for this
	.long	1			| version
1:	move.l	%a0,%a2			| stage 1 passes the boot block's address
	lea	__bss_start,%a0
	lea	__bss_end,%a1
2:	cmp.l	%a1,%a0
	bcc.s	3f
	clr.l	(%a0)+
	bra.s	2b
3:	move.l	%a2,bootblock
	jsr	main
	jmp	chain_macos

	.text

| int rom_read(void *pb): the Device Manager's _Read, synchronous
	.globl	rom_read
rom_read:
	movem.l	%d2/%a2,-(%sp)
	move.l	12(%sp),%a0
	.short	0xa002
	ext.l	%d0
	movem.l	(%sp)+,%d2/%a2
	rts

| int rom_osevent(int mask, void *event): _GetOSEvent; 0 if one came
	.globl	rom_osevent
rom_osevent:
	movem.l	%d2/%a2,-(%sp)
	move.l	12(%sp),%d0
	move.l	16(%sp),%a0
	.short	0xa031
	ext.l	%d0
	movem.l	(%sp)+,%d2/%a2
	rts

| void rom_readxpram(void *buf, int addr, int count): _ReadXPRam
	.globl	rom_readxpram
rom_readxpram:
	movem.l	%d2/%a2,-(%sp)
	move.l	12(%sp),%a0
	move.l	20(%sp),%d0
	swap	%d0
	move.w	18(%sp),%d0
	.short	0xa051
	movem.l	(%sp)+,%d2/%a2
	rts

| int fpu_probe(void): the size of the FPU's idle state frame, as Mac OS's
| Gestalt measures it: 0x18 is a 68881, 0x38 a 68882; -1 if there is none
| (the F-line trap). Linux checks fsave frames against the type we report.
	.chip	68k/68881
	.globl	fpu_probe
fpu_probe:
	move.w	%sr,-(%sp)
	ori.w	#0x0700,%sr
	movec	%vbr,%a1
	move.l	0x2c(%a1),-(%sp)		| F-line
	move.l	0x34(%a1),-(%sp)		| coprocessor protocol violation
	lea	.Lnofpu,%a0
	move.l	%a0,0x2c(%a1)
	move.l	%a0,0x34(%a1)
	move.l	%sp,probe_sp
	fmove.l	%fpcr,%d1			| leaves the FPU idle, not null
	fsave	-(%sp)
	moveq	#0,%d0
	move.b	1(%sp),%d0
	move.l	probe_sp,%sp
	clr.l	-(%sp)				| a null frame: reset, as it was
	frestore (%sp)+
	bra.s	1f
.Lnofpu:
	move.l	probe_sp,%sp
	moveq	#-1,%d0
1:	movec	%vbr,%a1
	move.l	(%sp)+,0x34(%a1)
	move.l	(%sp)+,0x2c(%a1)
	move.w	(%sp)+,%sr
	rts
	.chip	68030

	.globl	get_sp
get_sp:
	move.l	%sp,%d0
	rts

| void chain_macos(void): put back the registers and stack the ROM called
| the boot block with, and run System 7.1's own boot code at offset 0x8a.
	.globl	chain_macos
chain_macos:
	move.l	bootblock,%d0
	add.l	#0x8a,%d0
	move.l	%d0,chain_target
	movem.l	SAVE_AREA,%d0-%d7/%a0-%a7
	move.l	chain_target,-(%sp)
	rts

| void mmu_get(struct mmu_regs *m): TC, CRP, SRP, TT0, TT1
	.globl	mmu_get
mmu_get:
	move.l	4(%sp),%a0
	pmove	%tc,(%a0)
	pmove	%crp,4(%a0)
	pmove	%srp,12(%a0)
	pmove	%tt0,20(%a0)
	pmove	%tt1,24(%a0)
	rts

| void phys_begin(void): interrupts and caches off, the helper into low
| memory. Everything until phys_end may use phys_read.
	.globl	phys_begin
phys_begin:
	move.w	%sr,saved_sr
	ori.w	#0x0700,%sr
	movec	%cacr,%d0
	move.l	%d0,saved_cacr
	move.l	#0x0808,%d0		| caches off and emptied
	movec	%d0,%cacr
	lea	LOWFN,%a0		| keep what Mac OS has there
	lea	saved_low,%a1
	moveq	#15,%d0
1:	move.l	(%a0)+,(%a1)+
	dbra	%d0,1b
	lea	lowfn,%a0
	lea	LOWFN,%a1
	moveq	#(lowfn_end-lowfn)/2-1,%d0
2:	move.w	(%a0)+,(%a1)+
	dbra	%d0,2b
	lea	LOWOLD,%a1
	pmove	%tt0,(%a1)
	rts

| u32 phys_read(u32 addr): read a long from a physical address by making
| its 16 MB transparent, for reads only, just for one instruction. The
| helper runs from low memory, which is the same logically and physically,
| so it keeps running whether or not its own addresses are the ones made
| transparent; it touches no other memory.
	.globl	phys_read
phys_read:
	move.l	4(%sp),%a0
	move.l	%a0,%d0
	and.l	#0xff000000,%d0
	or.l	#0x00008607,%d0		| enabled, cache inhibited, reads, any FC
	move.l	%d0,LOWTT
	lea	LOWTT,%a1
	jsr	LOWFN
	rts

lowfn:
	pmove	(%a1),%tt0
	move.l	(%a0),%d0
	pmove	4(%a1),%tt0
	rts
lowfn_end:

	.globl	phys_end
phys_end:
	lea	saved_low,%a0
	lea	LOWFN,%a1
	moveq	#15,%d0
1:	move.l	(%a0)+,(%a1)+
	dbra	%d0,1b
	move.l	saved_cacr,%d0
	or.l	#0x0808,%d0		| empty the caches as they come back on
	movec	%d0,%cacr
	move.w	saved_sr,%sr
	rts

| void go_kernel(const struct piece *p, int n, u32 dst, u32 entry): leave
| Mac OS. The kernel image is n pieces of {logical, physical, length}. The
| mover and the list go to low memory, which is the same address logically
| and physically; the mover turns the MMU off there, copies the pieces in
| order to physical dst (each forwards or backwards as needed; load.c has
| checked none lands on a piece still to come), and jumps to the entry
| with 'Pen2' in d0, as Penguin does.
	.globl	go_kernel
go_kernel:
	ori.w	#0x0700,%sr
	move.l	4(%sp),%a0
	move.l	8(%sp),%d3
	move.l	12(%sp),%a3
	move.l	16(%sp),%a4
	lea	PIECES,%a1
	move.l	%d3,%d0
	mulu.w	#3,%d0
	bra.s	2f
1:	move.l	(%a0)+,(%a1)+
2:	dbra	%d0,1b
	lea	mover,%a0
	lea	MOVER,%a1
	move.w	#(mover_end-mover)/2-1,%d0
3:	move.w	(%a0)+,(%a1)+
	dbra	%d0,3b
	move.l	#0x0808,%d0		| caches off and emptied
	movec	%d0,%cacr
	lea	PIECES,%a2
	jmp	MOVER

mover:
	lea	MOVER_SCRATCH,%a0
	pmove	%tc,(%a0)
	bclr	#7,(%a0)
	pmove	(%a0),%tc		| the MMU is off from here on
	clr.l	(%a0)
	pmove	(%a0),%tt0
	pmove	(%a0),%tt1
	lea	MOVER_SCRATCH,%sp
5:	tst.l	%d3
	beq.s	4f
	move.l	4(%a2),%a0		| this piece's physical address
	move.l	8(%a2),%d2		| and length
	lea	12(%a2),%a2
	subq.l	#1,%d3
	move.l	%a3,%a1
	add.l	%d2,%a3
	lsr.l	#2,%d2
	beq.s	5b
	cmp.l	%a0,%a1
	bhi.s	2f
1:	move.l	(%a0)+,(%a1)+		| down: copy forwards
	subq.l	#1,%d2
	bne.s	1b
	bra.s	5b
2:	lea	(%a0,%d2.l*4),%a0	| up: copy backwards
	lea	(%a1,%d2.l*4),%a1
3:	move.l	-(%a0),-(%a1)
	subq.l	#1,%d2
	bne.s	3b
	bra.s	5b
4:	move.l	#0x0808,%d0
	movec	%d0,%cacr
	move.l	#0x50656e32,%d0		| 'Pen2'
	jmp	(%a4)
mover_end:

	.bss
	.align	4
	.globl	bootblock
bootblock:	.skip	4
chain_target:	.skip	4
saved_cacr:	.skip	4
saved_sr:	.skip	2
	.align	4
probe_sp:	.skip	4
	.align	4
saved_low:	.skip	64
