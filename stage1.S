| stage1.S - se30boot's boot block code.
|
| The Mac ROM reads the startup volume's two boot blocks and calls the code
| their entry branch points to. System 7.1's boot code is only 262 bytes
| (offsets 0x8a-0x18f) and the rest of the 1 KB is filler, so this goes in
| the filler at 0x190 and the installer points the entry branch here. We
| load stage 2 to STAGE2 with the boot drive's own driver and jump to it.
| If anything is wrong, or stage 2 is asked for Mac OS, the ROM's registers
| and stack are put back as they were and System 7.1's code runs unchanged.
|
| Assembled as position-independent code that starts at boot block 0x190.

	.equ	STAGE2,		0x00700000	| where stage 2 runs
	.equ	STAGE2_TOP,	0x00780000	| ... and how far it may reach
	.equ	SAVE,		0x006fff00	| the ROM's registers, for Mac OS
	.equ	MemTop,		0x108
	.equ	BootDrive,	0x210
	.equ	DrvQHead,	0x30a		| DrvQHdr + 2: first drive queue element
	.equ	ORIGINAL,	0x8a		| System 7.1's boot code
	.equ	HERE,		0x190		| where this file starts in the block

	.text

stage1:
	movem.l	%d0-%d7/%a0-%a6,-(%sp)		| on the ROM's stack until checked
	| Stage 2 needs [SAVE, STAGE2_TOP), below both MemTop and BufPtr (where
	| Mac OS's own things start), with neither the stack nor these boot
	| blocks in it. (The ROM puts the stack at 4 MB on an SE/30.)
	cmp.l	#STAGE2_TOP,MemTop
	bcs.w	early
	cmp.l	#STAGE2_TOP,0x10c		| BufPtr
	bcs.w	early
	cmp.l	#SAVE-0x4000,%sp
	bcs.s	1f
	cmp.l	#STAGE2_TOP+0x100,%sp
	bcs.w	early
1:	lea	%pc@(stage1),%a0
	cmp.l	#SAVE-0x400,%a0
	bcs.s	2f
	cmp.l	#STAGE2_TOP,%a0
	bcs.w	early
2:	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	movem.l	%d0-%d7/%a0-%a7,SAVE		| everything, SP included

	| the boot drive's driver, from the drive queue
	move.w	BootDrive,%d1
	move.l	DrvQHead,%a1
3:	move.l	%a1,%d0
	beq.w	macos
	cmp.w	6(%a1),%d1			| dQDrive
	beq.s	2f
	move.l	(%a1),%a1
	bra.s	3b
2:	move.w	8(%a1),%d2			| dQRefNum

	| _Read stage 2 (offset and length from the parameters below)
	lea	-64(%sp),%sp
	move.l	%sp,%a0
	moveq	#15,%d0
4:	clr.l	(%a0)+
	dbra	%d0,4b
	move.l	%sp,%a0
	move.w	%d1,22(%a0)			| ioVRefNum: the drive
	move.w	%d2,24(%a0)			| ioRefNum: its driver
	move.l	#STAGE2,32(%a0)			| ioBuffer
	move.l	%pc@(s2_length),36(%a0)		| ioReqCount
	move.w	#1,44(%a0)			| ioPosMode: from the start
	move.l	%pc@(s2_offset),46(%a0)		| ioPosOffset, within the volume
	.short	0xa002				| _Read
	lea	64(%sp),%sp
	tst.w	%d0
	bne.w	macos
	cmp.l	#0x53453342,STAGE2+2		| 'SE3B' after stage 2's branch
	bne.w	macos
	lea	%pc@(stage1-HERE),%a0		| the boot block, for stage 2
	jmp	STAGE2

	| Mac OS: put everything back and run System 7.1's boot code
macos:
	movem.l	SAVE,%d0-%d7/%a0-%a7
	jmp	%pc@(stage1-HERE+ORIGINAL)
early:
	movem.l	(%sp)+,%d0-%d7/%a0-%a6
	jmp	%pc@(stage1-HERE+ORIGINAL)

	| filled in by the installer
	.org	0x3f0 - HERE

s2_offset:	.long	0		| bytes from the start of the volume
s2_length:	.long	0
	.ascii	"se30boot"		| marks the block as ours
