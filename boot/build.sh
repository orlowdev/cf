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

# Assemble the committed seed IL with the pinned qbe (the trust chain's qbe step).
"$qbe" -t arm64_apple -o "$tmp/prog.s" "$seed/cf.qbe"
# Compile cf's C bridge to the embedded QBE and gather the embeddable objects (all but qbe's main.o,
# whose `main` would collide with cf's entry).
cc -std=c99 -I "$qbedir" -c "$root/boot/qbe_embed.c" -o "$tmp/qbe_embed.o"
embed=$(ls "$qbedir"/*.o "$qbedir"/*/*.o | grep -v '/main\.o$')
# Link cf: the floor, the compiler program, the embedded QBE, and the bridge.
# shellcheck disable=SC2086
cc -nostdlib -lSystem -Wl,-e,_start -o "$out" "$seed/floor.s" "$tmp/prog.s" $embed "$tmp/qbe_embed.o"
echo "build: ok -> $out"
