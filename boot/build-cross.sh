#!/bin/sh
# build-cross.sh <arch> — cross-build a self-contained linux cf from a non-linux host (arch: arm64 |
# riscv64). Produces var/cf-linux-<arch>.
#
# ⚠ EXPERIMENTAL — NOT yet validated end to end. build.sh (the trusted build) is darwin-native; there
# is no committed linux build recipe. This script is the first cut at one, used by the CI
# build-binaries workflow under `continue-on-error`. The hard, unproven part is the final link: cf's
# floor owns `_start` (freestanding), while the embedded QBE is C that uses libc (malloc, stderr). We
# link a static musl via `zig cc` and keep the floor's `_start` (`-nostartfiles`), so musl's own
# startup/TLS init is skipped — that MAY fault for libc calls that need it. If a produced binary
# crashes on linux, this is the place to look (candidate fixes: have the linux floor jump through
# musl's `__libc_start_main`, or build QBE against a libc that needs no init). Treat any success as
# provisional until a produced cf is run on real hardware/qemu.
set -eu

arch="${1:?usage: build-cross.sh <arm64|riscv64>}"
case "$arch" in
	arm64)   ztarget=aarch64-linux-musl; qtarget=arm64; cftarget=linux-arm64 ;;
	riscv64) ztarget=riscv64-linux-musl; qtarget=rv64;  cftarget=linux-riscv64 ;;
	*) echo "build-cross: unknown arch '$arch' (expected arm64 or riscv64)" >&2; exit 2 ;;
esac

command -v zig >/dev/null 2>&1 || { echo "build-cross: zig not found on PATH" >&2; exit 127; }

root=$(cd "$(dirname "$0")/.." && pwd)
qbedir="$root/boot/vendor/qbe"
qbe="$qbedir/qbe"
seed="$root/boot/seed"
out="$root/var/cf-$cftarget"

if [ ! -f "$seed/cf.qbe" ] || [ ! -f "$seed/floor.s" ]; then
	echo "build-cross: seed missing at $seed (cf.qbe / floor.s)" >&2
	exit 1
fi

# The vendored QBE: the standalone `qbe` (a cross-assembler of cf's IL — it emits any target's asm
# regardless of host) plus its C sources, which we recompile FOR the linux target below.
make -C "$qbedir" -s

mkdir -p "$root/var"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cf-cross.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

# Stage 1 — a HOST cf0 from the seed (native `cc`, exactly as build.sh), used only to cross-emit cf's
# linux IL + floor. The host embed set is the QBE objects built for the host by `make` above.
cc -std=c99 -I "$qbedir" -c "$root/boot/qbe_embed.c" -o "$tmp/qbe_embed_host.o"
hostembed=$(ls "$qbedir"/*.o "$qbedir"/*/*.o | grep -v '/main\.o$')
"$qbe" -t arm64_apple -o "$tmp/cf0.prog.s" "$seed/cf.qbe"
# shellcheck disable=SC2086
cc -nostdlib -lSystem -Wl,-e,_start -o "$tmp/cf0" "$seed/floor.s" "$tmp/cf0.prog.s" $hostembed "$tmp/qbe_embed_host.o"

# Stage 2 — cf0 emits cf's source as LINUX IL + a linux floor (embeds ON, so lib/std is baked in and
# the shipped cf resolves `std::…` from nothing on disk). Then the standalone qbe assembles the IL to
# the target's asm (`-t arm64` / `-t rv64`, the no-underscore ELF form the linux floor expects).
"$tmp/cf0" "$root/boot/src/cf.cf" --target "$cftarget" "$tmp/cf.qbe" "$tmp/cf.floor.s"
"$qbe" -t "$qtarget" -o "$tmp/cf.prog.s" "$tmp/cf.qbe"

# Cross-compile QBE's C runtime (+ cf's bridge) for the linux target with zig's bundled musl, minus
# qbe's own main.o (its `main` would collide with cf's entry).
mkdir -p "$tmp/obj"
n=0
for c in "$qbedir"/*.c "$qbedir"/*/*.c; do
	case "$c" in */main.c) continue ;; esac
	[ -f "$c" ] || continue
	zig cc -target "$ztarget" -std=c99 -O2 -I "$qbedir" -c "$c" -o "$tmp/obj/q$n.o"
	n=$((n + 1))
done
zig cc -target "$ztarget" -std=c99 -O2 -I "$qbedir" -c "$root/boot/qbe_embed.c" -o "$tmp/obj/embed.o"

# Link static: the floor (owns `_start`) + cf's program asm + the QBE object set. `-nostartfiles`
# keeps the floor's entry (see the EXPERIMENTAL note above about musl init).
# shellcheck disable=SC2046
zig cc -target "$ztarget" -static -nostartfiles -Wl,-e,_start -o "$out" \
	"$tmp/cf.floor.s" "$tmp/cf.prog.s" $(ls "$tmp/obj"/*.o)

echo "build-cross: ok -> $out"
