#!/bin/sh
# Build cf — the self-hosted C! compiler (the keeper) — from a committed per-platform SEED.
#
#   build.sh [target]     target: darwin-arm64 | darwin-amd64 | linux-arm64 | linux-amd64 | linux-riscv64
#                         (default: host)
#
# The permanent trust root is `boot/seed_<os>/` — the QBE IL + freestanding floor that cf emits FOR
# ITS OWN SOURCE (a self-reproducing fixpoint). The seed is per-OS because the compiler's own I/O bakes
# that OS's syscall numbers into the IL (comptime module selection), not only into the asm floor; the
# two linux arches SHARE one IL (generic ABI) and differ only in the floor. Building cf is the
# `qbe -> cc` tail applied to the seed — only the vendored `qbe` and a system C toolchain are in the
# loop, no genesis compiler.
#
# The shipped cf ALSO embeds QBE: qbe's objects (minus its own main.o) plus cf's C bridge
# (boot/qbe_embed.c) are linked in, so cf translates IL->asm in-process. QBE uses libc; cf's floor owns
# `_start` (freestanding), so we link libc for QBE WITHOUT its crt startup (-lSystem on darwin, static
# -lc on linux). darwin and linux-arm64 build natively from their own seed; linux-riscv64 cross-builds
# on a linux-arm64 host (shared IL, riscv floor, a riscv cross toolchain for QBE's C).
#
# After editing cf's `.cf` source, regenerate BOTH seeds with boot/reseed.sh, then commit them.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
qbedir="$root/boot/vendor/qbe"
qbe="$qbedir/qbe"
out="$root/var/cf"

case "$(uname -s)/$(uname -m)" in
	Darwin/arm64)              host=darwin-arm64 ;;
	Darwin/x86_64)             host=darwin-amd64 ;;
	Linux/aarch64 | Linux/arm64) host=linux-arm64 ;;
	Linux/x86_64)              host=linux-amd64 ;;
	Linux/riscv64)             host=linux-riscv64 ;;
	*) echo "build: unsupported host $(uname -s)/$(uname -m)" >&2; exit 1 ;;
esac
target="${1:-$host}"

# Per-platform knobs. `seed` is the OS seed dir; `qt` the qbe `-t` target; `fa` the floor's arch name
# (floor.$fa.s in the seed); `cc`/`link`/`libs` the C toolchain + link recipe (floor owns `_start`);
# `march` extra flags threaded into BOTH the target C compile and the link (so a cross build targets
# the right machine — e.g. `-arch x86_64` when an Apple-Silicon host builds darwin-amd64).
knobs() { # <platform>  ->  sets seed qt fa cc link libs march
	march=""
	case "$1" in
		darwin-arm64)
			seed="$root/boot/seed_mac"; qt=arm64_apple; fa=arm64
			cc="cc"; link="-nostdlib -lSystem -Wl,-e,_start"; libs="" ;;
		darwin-amd64)
			# Shares the mac seed IL (darwin arm64/amd64 use identical BSD syscall numbers — the amd64
			# `syscall` class bit lives in the floor trampoline, not the IL); only the floor asm differs.
			# `-arch x86_64` (in `march`) lets an Apple-Silicon host cross-compile the embedded QBE
			# objects AND cross-assemble+link the Mach-O; run the result via Rosetta 2.
			seed="$root/boot/seed_mac"; qt=amd64_apple; fa=amd64
			cc="cc"; link="-nostdlib -lSystem -Wl,-e,_start"; libs=""; march="-arch x86_64" ;;
		linux-arm64)
			# musl (not glibc): static musl links cleanly under the floor's own `_start`, where glibc's
			# static libc.a drags in crt/dynamic-loader machinery it never gets. The linux floor calls
			# musl's `__init_libc` so the embedded QBE's libc works. Yields a portable static binary.
			# `-u __init_libc` force-pulls musl's strong init over the floor's weak no-op (see the floor);
			# `-lgcc` supplies the soft-float long-double helpers musl's printf uses (__addtf3, …);
			# `-no-pie` because the floor's `_start` does no PIE self-relocation (some musl gccs default PIE).
			seed="$root/boot/seed_linux"; qt=arm64; fa=arm64
			cc="${LINUX_CC:-musl-gcc}"; link="-nostdlib -static -no-pie -Wl,-e,_start -Wl,-u,__init_libc"; libs="-lc -lgcc" ;;
		linux-riscv64)
			seed="$root/boot/seed_linux"; qt=rv64; fa=riscv64
			cc="${RISCV_CC:-riscv64-linux-musl-gcc}"; link="-nostdlib -static -no-pie -Wl,-e,_start -Wl,-u,__init_libc"; libs="-lc -lgcc" ;;
		*) echo "build: unknown platform $1" >&2; exit 1 ;;
	esac
}

# Host toolchain (for cf0 + the vendored QBE), resolved from the host knobs so the whole build uses one
# C compiler — cc on darwin, musl-gcc on linux (matching the embedded QBE objects to the libc we link).
knobs "$host"; hseed="$seed"; hqt="$qt"; hfa="$fa"; hcc="$cc"; hlink="$link"; hlibs="$libs"

# The embedded QBE's C is built OPTIMIZED and without debug info (QBE's own Makefile defaults to
# `-g` and no `-O`): `-O2` makes cf's IL->asm step faster, and dropping `-g` shrinks the binary. This
# only affects how cf RUNS, never the IL it emits — the seed/fixpoint is unchanged.
cflags="-std=c99 -O2"

# The vendored QBE: the standalone `qbe` (a cross-assembler of cf's IL — emits any target's asm) plus
# its object set, built for the HOST with the host compiler. Idempotent.
make -C "$qbedir" -s CC="$hcc" CFLAGS="$cflags"
host_embed=$(ls "$qbedir"/*.o "$qbedir"/*/*.o | grep -v '/main\.o$')

mkdir -p "$root/var"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cf-build.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

# cf's C bridge, compiled for the HOST (for cf0) and, when cross-building, for the TARGET (below).
"$hcc" $cflags -I "$qbedir" -c "$root/boot/qbe_embed.c" -o "$tmp/qbe_embed_host.o"

# --- Stage 1: assemble the HOST seed into the bootstrap compiler cf0 (runs on this machine). ---
for f in "$hseed/cf.qbe" "$hseed/floor.$hfa.s"; do
	[ -f "$f" ] || { echo "build: seed missing: $f" >&2; exit 1; }
done
"$qbe" -t "$hqt" -o "$tmp/cf0.prog.s" "$hseed/cf.qbe"
# shellcheck disable=SC2086
$hcc $hlink -o "$tmp/cf0" "$hseed/floor.$hfa.s" "$tmp/cf0.prog.s" $host_embed "$tmp/qbe_embed_host.o" $hlibs

# --- Stage 2: cf0 emits cf's IL+floor for the TARGET (embeds ON — lib/std baked in). ---
knobs "$target"
"$tmp/cf0" "$root/boot/src/cf.cf" --target "$target" "$tmp/cf.qbe" "$tmp/cf.floor.s"
"$qbe" -t "$qt" -o "$tmp/cf.prog.s" "$tmp/cf.qbe"

# The embedded QBE objects must match the TARGET arch. Native (target arch == host arch) reuses the
# host object set; a cross build (riscv on arm-linux) recompiles QBE's C with the target toolchain.
if [ "$target" = "$host" ]; then
	tembed="$host_embed"; tqembed="$tmp/qbe_embed_host.o"
else
	# Recompile FOR the target exactly the source set the host objects came from (so `tools/` and
	# `main.o`, already excluded from $host_embed, stay excluded), giving each a unique object name.
	mkdir -p "$tmp/tobj"; i=0
	for o in $host_embed; do
		# shellcheck disable=SC2086
		"$cc" $march $cflags -I "$qbedir" -c "${o%.o}.c" -o "$tmp/tobj/q$i.o"
		i=$((i + 1))
	done
	# shellcheck disable=SC2086
	"$cc" $march $cflags -I "$qbedir" -c "$root/boot/qbe_embed.c" -o "$tmp/tobj/embed.o"
	tembed=$(ls "$tmp/tobj"/q*.o); tqembed="$tmp/tobj/embed.o"
fi

# --- Link the target cf: floor (owns _start) + program asm + embedded QBE + bridge. ---
# shellcheck disable=SC2086
$cc $march $link -o "$out" "$tmp/cf.floor.s" "$tmp/cf.prog.s" $tembed "$tqembed" $libs

# Release builds (`CF_STRIP=1`) strip the symbol table for a smaller artifact — the objects already
# carry no `-g`, so this is the last of the size. Local builds keep symbols (cf is debuggable with
# lldb). riscv needs its cross `strip`; darwin and native linux-arm64 use the host `strip`.
if [ -n "${CF_STRIP:-}" ]; then
	case "$target" in
		linux-riscv64) "${RISCV_STRIP:-riscv64-linux-musl-strip}" "$out" ;;
		*) strip "$out" ;;
	esac
fi
echo "build: ok -> $out ($target)"
