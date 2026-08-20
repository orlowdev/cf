/* Bare-metal AArch64 (ELF) linker script for `cf --target bare-arm64`.
 *
 * The cf floor is freestanding: `_start` (the entry) points SP at `_stack_top`, seeds `cf_page` over
 * the static 1 MiB `cf_root` BSS block, calls `main`, then requests a QEMU semihosting SYS_EXIT with
 * main's return as the status (spins if semihosting is off) — no libc, no dynamic loader, no OS
 * syscalls. This script places the flat image at a fixed load address, reserves the machine stack,
 * and wires the entry symbol. Assemble the cf/qbe output as
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

	/* the machine stack — SP is undefined at reset, so `_start` sets it from `_stack_top`.
	 * grows down from the top of this 64 KiB reservation. */
	. = ALIGN(16);
	. = . + 0x10000;
	_stack_top = .;

	/* strip toolchain metadata the freestanding image does not need */
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
