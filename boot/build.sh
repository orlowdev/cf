#!/bin/sh
# Build cf — the self-hosted C! compiler (the keeper) — from the committed SEED.
#
# The genesis C tool (cfcc) is gone (see the cf-genesis-strategy): the permanent trust
# root is `boot/seed/` — the QBE IL + freestanding floor that cf emits FOR ITS OWN
# SOURCE (`S2`, a self-reproducing fixpoint, DDC-verified across two independent C
# compilers). Building cf is the `qbe -> cc` tail applied to that seed — no genesis C
# compiler is in the loop, only the vendored `qbe` (boot/vendor/qbe) and the system `cc`.
#
# The shipped cf ALSO embeds QBE: qbe's objects (minus its own main.o) plus cf's C bridge
# (boot/qbe_embed.c, exposing `cf_qbe_run`) are linked in, so cf translates IL->asm in-process
# and needs no external qbe at use-time. QBE uses libc, resolved via -lSystem (already present).
#
# After editing cf's `.cf` source, regenerate the seed with boot/reseed.sh (which rebuilds from
# the OLD seed, recompiles the new source, and re-verifies the fixpoint), then commit it.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
qbedir="$root/boot/vendor/qbe"
qbe="$qbedir/qbe"
seed="$root/boot/seed"
out="$root/var/cf"

# Build the vendored QBE: the standalone `qbe` (assembles the seed IL) and the object set (embedded
# into cf). Idempotent — make rebuilds only what changed.
make -C "$qbedir" -s

if [ ! -f "$seed/cf.qbe" ] || [ ! -f "$seed/floor.s" ]; then
	echo "build: seed missing at $seed (cf.qbe / floor.s)" >&2
	exit 1
fi

mkdir -p "$root/var"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cf-build.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

# The build is TWO stages because the committed seed is a std-LESS compiler (reseed emits it with
# `--skip-embeds`, so the stdlib never bloats the trust-seed). Stage 1 assembles that seed into a
# bootstrap compiler `cf0`; stage 2 has cf0 recompile the compiler WITHOUT the flag, baking lib/std
# into the shipped, self-contained `var/cf`. So a stdlib edit reflows into cf via stage 2 — no reseed.

# Compile cf's C bridge to the embedded QBE and gather the embeddable objects (all but qbe's main.o,
# whose `main` would collide with cf's entry). Both generations link the same set.
cc -std=c99 -I "$qbedir" -c "$root/boot/qbe_embed.c" -o "$tmp/qbe_embed.o"
embed=$(ls "$qbedir"/*.o "$qbedir"/*/*.o | grep -v '/main\.o$')

# One IL+floor -> linked cf binary: qbe assembles the IL, then cc links the floor, the compiler
# program, the embedded QBE object set, and the C bridge.
link_cf() { # <in.qbe> <in.floor.s> <out-bin>
	"$qbe" -t arm64_apple -o "$3.prog.s" "$1"
	# shellcheck disable=SC2086
	cc -nostdlib -lSystem -Wl,-e,_start -o "$3" "$2" "$3.prog.s" $embed "$tmp/qbe_embed.o"
}

# Stage 1: assemble the committed (std-less) seed into the bootstrap compiler cf0.
link_cf "$seed/cf.qbe" "$seed/floor.s" "$tmp/cf0"

# Stage 2: cf0 emits the compiler's IL+floor with embedding ON (no --skip-embeds), baking lib/std
# (read from disk here) into the output; assemble + link that into the self-contained var/cf.
"$tmp/cf0" "$root/boot/src/cf.cf" "$tmp/cf.qbe" "$tmp/cf.floor.s"
link_cf "$tmp/cf.qbe" "$tmp/cf.floor.s" "$out"
echo "build: ok -> $out"
