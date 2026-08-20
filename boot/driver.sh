#!/bin/sh
# Assemble a cf-compiled program end to end.
#
# cf stops at two text artifacts: the QBE IL (`prog.qbe`, user code) and the asm floor (`floor.s`,
# `_start` + the arena runtime, emitted verbatim since QBE has no `svc`). This driver runs the
# `qbe`→assemble→link tail, branching on the target:
#
#   driver.sh [--target <os>-<arch>] <input.cf> <out-binary>
#
#   darwin-arm64 (default): qbe -t arm64_apple, then `cc -nostdlib -lSystem -Wl,-e,_start` (Mach-O;
#     darwin needs -lSystem present for the loader even with no libc symbol used). Same tail as
#     boot/build.sh uses to assemble the seed into cf itself.
#   bare-arm64: a freestanding ELF image — cf emits an ELF floor (SysV symbols, `:lo12:` relocs),
#     qbe -t arm64 the program, clang assembles both as AArch64 ELF, and an ELF linker links them
#     with boot/target/bare-arm64.ld (no libc, no loader). clang's own driver links Mach-O on darwin, so
#     the LINK step needs ld.lld / cross-binutils; absent one, the driver emits the ELF objects and
#     prints the finishing link command instead of failing.
#
# The `!` allocation algebra and the root-page rule are validated unconditionally by cf.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cf="$root/var/cf"
qbe="$root/opt/qbe/qbe"

target=darwin-arm64
if [ "${1:-}" = "--target" ]; then
	target=$2
	shift 2
fi
if [ "$#" -ne 2 ]; then
	echo "usage: driver.sh [--target <os>-<arch>] <input.cf> <out-binary>" >&2
	exit 2
fi
in=$1
out=$2

tmp=$(mktemp -d "${TMPDIR:-/tmp}/cf-driver.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

if [ "$target" = "bare-arm64" ]; then
	"$cf" --target bare-arm64 "$in" "$tmp/prog.qbe" "$tmp/floor.s"
	"$qbe" -t arm64 -o "$tmp/prog.s" "$tmp/prog.qbe"
	# assemble both units as AArch64 ELF — clang carries the aarch64 backend, so this works on a
	# stock host and validates the emitted ELF asm.
	clang -target aarch64-none-elf -c "$tmp/floor.s" -o "$tmp/floor.o"
	clang -target aarch64-none-elf -c "$tmp/prog.s" -o "$tmp/prog.o"
	link=""
	if command -v ld.lld >/dev/null 2>&1; then link=ld.lld; fi
	if command -v aarch64-none-elf-ld >/dev/null 2>&1; then link=aarch64-none-elf-ld; fi
	if [ -n "$link" ]; then
		"$link" -T "$root/boot/target/bare-arm64.ld" -o "$out" "$tmp/floor.o" "$tmp/prog.o"
	else
		cp "$tmp/floor.o" "${out}.floor.o"
		cp "$tmp/prog.o" "${out}.prog.o"
		echo "driver: assembled AArch64 ELF objects, but no ELF linker (ld.lld) on this host." >&2
		echo "        Emitted ${out}.floor.o and ${out}.prog.o. Finish with an ELF linker:" >&2
		echo "          ld.lld -T $root/boot/target/bare-arm64.ld -o $out ${out}.floor.o ${out}.prog.o" >&2
	fi
else
	"$cf" --target "$target" "$in" "$tmp/prog.qbe" "$tmp/floor.s"
	"$qbe" -t arm64_apple -o "$tmp/prog.s" "$tmp/prog.qbe"
	cc -nostdlib -lSystem -Wl,-e,_start -o "$out" "$tmp/floor.s" "$tmp/prog.s"
fi
