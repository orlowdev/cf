#!/bin/sh
# Assemble a cf-compiled program end to end — a DEV/BOOTSTRAP helper and the corpus test harness.
#
# END USERS no longer need this: the shipped cf is self-contained (`cf <input.cf> -o <output>` runs
# the embedded QBE and spawns `cc` itself). This driver drives cf's LEGACY 3-path form (emit QBE IL +
# asm floor to two files) through the external `qbe`→assemble→link tail — kept for the regression
# suite and cross/bare targets, branching on the target:
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
#     prints the finishing link command instead of failing. Boot the result under
#     `qemu-system-aarch64 -machine virt -cpu cortex-a57 -nographic -semihosting -kernel <out>`:
#     the floor's semihosting SYS_EXIT makes qemu exit with main's return as its status.
#
# The `!` allocation algebra and the root-page rule are validated unconditionally by cf.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cf="$root/var/cf"
qbe="$root/boot/vendor/qbe/qbe"

# collect leading `--flag value` pairs: `target` steers the qbe/link tail, and every flag is also
# forwarded to cf verbatim (`$flags`) so `--root-size` etc. reach the compiler.
target=darwin-arm64
flags=""
while [ "$#" -gt 0 ]; do
	case "$1" in
	--target) target=$2; flags="$flags --target $2"; shift 2 ;;
	--root-size) flags="$flags --root-size $2"; shift 2 ;;
	*) break ;;
	esac
done
if [ "$#" -ne 2 ]; then
	echo "usage: driver.sh [--target <os>-<arch>] [--root-size <bytes>] <input.cf> <out-binary>" >&2
	exit 2
fi
in=$1
out=$2

tmp=$(mktemp -d "${TMPDIR:-/tmp}/cf-driver.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

if [ "$target" = "bare-arm64" ]; then
	# shellcheck disable=SC2086
	"$cf" $flags "$in" "$tmp/prog.qbe" "$tmp/floor.s"
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
	# shellcheck disable=SC2086
	"$cf" $flags "$in" "$tmp/prog.qbe" "$tmp/floor.s"
	"$qbe" -t arm64_apple -o "$tmp/prog.s" "$tmp/prog.qbe"
	cc -nostdlib -lSystem -Wl,-e,_start -o "$out" "$tmp/floor.s" "$tmp/prog.s"
fi
