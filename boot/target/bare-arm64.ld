/* Bare-metal AArch64 (ELF) linker script for `cf --target bare-arm64`.
 *
 * The cf floor is freestanding: `_start` (the entry) seeds `cf_page` over the static 1 MiB `cf_root`
 * BSS block, calls `main`, then halts — no libc, no dynamic loader, no syscalls. This script places
 * the flat image at a fixed load address and wires the entry symbol. Assemble the cf/qbe output as
 * ELF (`clang -target aarch64-none-elf` / `aarch64-elf-as`) and link with `ld -T bare-arm64.ld`
 * (or `ld.lld`). 0x40080000 is the conventional AArch64 load address (e.g. QEMU `-machine virt`);
 * override it for your board.
 */
ENTRY(_start)

SECTIONS {
	. = 0x40080000;

	.text : {
		*(.text._start)   /* keep the entry first */
		*(.text*)
	}

	.rodata : { *(.rodata*) }
	.data   : { *(.data*) }

	.bss (NOLOAD) : {
		*(.bss*)
		*(COMMON)         /* cf_root (the 1 MiB static root) lands here */
	}

	/* strip toolchain metadata the freestanding image does not need */
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
