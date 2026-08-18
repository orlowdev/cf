#!/bin/sh
# Build cf0 — the self-hosted C! compiler (the keeper) — from the committed SEED.
#
# The genesis C tool (cfcc) is gone (see the cf0-genesis-strategy): the permanent trust
# root is `boot/seed/` — the QBE IL + freestanding floor that cf0 emits FOR ITS OWN
# SOURCE (`S2`, a self-reproducing fixpoint, DDC-verified across two independent C
# compilers). Building cf0 is therefore just the `qbe -> cc` tail applied to that seed;
# no genesis C compiler is in the loop, only the vendored `qbe` and the system `cc`
# (darwin needs -lSystem present for the loader even with no libc symbol used).
#
# After editing cf0's `.cf` source, regenerate the seed with boot/reseed.sh (which
# rebuilds from the OLD seed, recompiles the new source, and re-verifies the fixpoint),
# then commit the updated seed alongside the source.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
qbe="$root/opt/qbe/qbe"
seed="$root/boot/seed"
out="$root/var/cf0"

if [ ! -x "$qbe" ]; then
	echo "build: vendored qbe missing at $qbe" >&2
	echo "       run boot/fetch-qbe.sh first" >&2
	exit 1
fi
if [ ! -f "$seed/cf0.qbe" ] || [ ! -f "$seed/floor.s" ]; then
	echo "build: seed missing at $seed (cf0.qbe / floor.s)" >&2
	exit 1
fi

mkdir -p "$root/var"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/cf0-build.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

"$qbe" -t arm64_apple -o "$tmp/prog.s" "$seed/cf0.qbe"
cc -nostdlib -lSystem -Wl,-e,_start -o "$out" "$seed/floor.s" "$tmp/prog.s"
echo "build: ok -> $out"
