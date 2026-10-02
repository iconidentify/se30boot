/* stage 2 runs where stage 1 reads it */
ENTRY(_start)
SECTIONS
{
	. = 0x00700000;
	.text : {
		*(.text.head)
		*(.text .text.*)
		*(.rodata .rodata.*)
		. = ALIGN(4);
		*(.data .data.*)
		. = ALIGN(512);
		_load_end = .;
	}
	.bss : {
		__bss_start = .;
		*(.bss .bss.* COMMON)
		. = ALIGN(4);
		__bss_end = .;
	}
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
ASSERT(__bss_end <= 0x00780000, "stage 2 is too big")
