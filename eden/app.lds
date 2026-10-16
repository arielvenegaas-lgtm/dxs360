/* Eden360 app.lds: libXenon's default script with one change.
 *
 * The frontend reserves a large guest-RAM array for the emulation core. In the
 * stock script that array is an ordinary .bss object, and because the 360's
 * EHCI USB controller only reaches the low part of RAM, growing .bss pushes
 * libXenon's USB DMA structures (usb_os.c heapspace[], and every malloc'd
 * transfer buffer that follows the heap) out of reach, so USB mass storage
 * stops enumerating. Empirically a ~10 MB image mounts USB and a ~31 MB image
 * does not.
 *
 * So the guest RAM is emitted into its own NOLOAD section placed *above* a
 * capped heap instead of into .bss. Low .bss (and therefore the heap and every
 * EHCI buffer) then stays at the same address as the known-working 0.3.0
 * build, while guest RAM can be any size. The script is otherwise identical to
 * $(DEVKITXENON)/app.lds.
 */
ENTRY(_start)
SECTIONS
{
	. = 0x80000000;
	.text : {  *(.except*); *(.pagetable); pagetable_end = .; *(.init*); *(.fini*); *(.text*); }
	.elfldr : { elfldr_start = .;*(.elfldr); elfldr_end = .;}

	.data : { *(.data*) }
	.sdata : { *(.sdata*) }
	.rodata : { *(.rodata*)}

	.ctors : { __CTOR_LIST__ = .; *(.ctors); __CTOR_END__ = .; }
	.dtors : {__DTOR_LIST__ = .; *(.dtors); __DTOR_END__ = .; }

	.eh_frame_hdr : { *(.eh_frame_hdr) }
	.eh_frame ALIGN(0x4): { PROVIDE (__eh_frame_start = .); KEEP (*(.eh_frame)) LONG (0); }
	.gcc_except_table :  { *(.gcc_except_table .gcc_except_table.*) }

	bss_start = .;
	.bss : { *(.bss.beginning.lower); *(.bss.beginning.upper); *(.bss*) }
	.sbss : { *(.sbss .sbss.*) }
	bss_end = .;

	/* Heap stays low so libXenon's EHCI DMA allocations land where the
	 * controller can reach them; 48 MB is far more than the frontend uses. */
	heap_begin = .;
	. = 0x83000000;
	heap_end = .;

	/* Guest RAM for the emulation core, kept above the heap so its size can
	 * grow without moving the USB DMA structures out of the reachable window.
	 * NOLOAD: zeroed memory, occupies no space in the ELF file. */
	.guestram (NOLOAD) : { *(.guestram) }

	. = 0x9E000000;
	__libc_stack_end = . ;
	. = 0xa0000000;
}
