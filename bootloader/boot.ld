/* Bootloader: first 8 KiB of the CH32V203C8U6 flash; the last 16 bytes hold the version record.
   The chip starts at its alias address 0x00000000 and the code addresses RAM relative to the program counter,
   so the bootloader is linked at the alias (the image is still programmed at 0x08000000). The application
   slots are entered through their 0x080xxxxx addresses and linked there. */
ENTRY( _start )

__stack_size = 1536;
PROVIDE( _stack_size = __stack_size );

MEMORY
{
	FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 8K - 16
	VERREC (r) : ORIGIN = 0x00001FF0, LENGTH = 16
	/* The top 16 bytes of RAM carry the "stay in the bootloader" request across a reset. */
	RAM (xrw)  : ORIGIN = 0x20000000, LENGTH = 20K - 16
}

SECTIONS
{
	.init :
	{
		. = ALIGN(4);
		KEEP(*(SORT_NONE(.init)))
		. = ALIGN(4);
	} >FLASH

	.vector :
	{
		PROVIDE( _vector_base = . );
		*(.vector);
		. = ALIGN(64);
	} >FLASH

	.text :
	{
		. = ALIGN(4);
		*(.text)
		*(.text.*)
		*(.rodata)
		*(.rodata*)
		. = ALIGN(4);
	} >FLASH

	.bootinfo : { KEEP(*(.bootinfo)) } >VERREC

	.dalign :
	{
		. = ALIGN(4);
		PROVIDE(_data_vma = .);
	} >RAM AT>FLASH

	.dlalign :
	{
		. = ALIGN(4);
		PROVIDE(_data_lma = .);
	} >FLASH

	.data :
	{
		*(.data .data.*)
		. = ALIGN(8);
		PROVIDE( __global_pointer$ = . + 0x800 );
		*(.sdata .sdata.*)
		*(.srodata.cst16)
		*(.srodata.cst8)
		*(.srodata.cst4)
		*(.srodata.cst2)
		*(.srodata .srodata.*)
		. = ALIGN(4);
		PROVIDE( _edata = .);
	} >RAM AT>FLASH

	.bss :
	{
		. = ALIGN(4);
		PROVIDE( _sbss = .);
		*(.sbss*)
		*(.bss*)
		*(COMMON*)
		. = ALIGN(4);
		PROVIDE( _ebss = .);
	} >RAM

	.stack ORIGIN(RAM) + LENGTH(RAM) - __stack_size :
	{
		. = ALIGN(4);
		PROVIDE(_susrstack = . );
		. = . + __stack_size;
		PROVIDE( _eusrstack = .);
	} >RAM
}
