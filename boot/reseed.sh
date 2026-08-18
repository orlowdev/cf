#!/bin/sh
# Regenerate the committed seed after editing cf's `.cf` source, and VERIFY the fixpoint.
#
# The self-hosting maintenance loop (seed_subset §3): cf's binary is built from the seed
# (`boot/seed/`), so a source edit leaves the seed stale. This rebuilds cf from the OLD
# seed, has THAT cf recompile the (new) source into a fresh seed, then confirms the fresh
# seed is a true fixpoint — a cf built from it recompiles the source to the SAME thing.
# Only then is the new seed written. A non-fixpoint (the emitted IL doesn't reproduce)
# aborts without touching the committed seed.
#
# Note the bootstrap constraint: the OLD cf must already be able to compile the NEW source
# (you can only add a language feature cf already accepts). A feature that changes cf's
# own accepted subset needs a transitional two-step reseed.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
qbe="$root/opt/qbe/qbe"
seed="$root/boot/seed"
src="$root/boot/src/cf.cf"

asm() { # <in.qbe> <in.floor.s> <out-bin>
	"$qbe" -t arm64_apple -o "$3.prog.s" "$1"
	cc -nostdlib -lSystem -Wl,-e,_start -o "$3" "$2" "$3.prog.s"
}

tmp=$(mktemp -d "${TMPDIR:-/tmp}/cf-reseed.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

# 1. cf₀ from the CURRENT (committed) seed.
asm "$seed/cf.qbe" "$seed/floor.s" "$tmp/cf_old"

# 2. cf₀ recompiles the (possibly edited) source → the candidate seed.
"$tmp/cf_old" "$src" "$tmp/new.qbe" "$tmp/new.floor.s"

# 3. Fixpoint check: build cf₁ from the candidate seed, recompile the source, and require
#    the emission to reproduce the candidate byte-for-byte.
asm "$tmp/new.qbe" "$tmp/new.floor.s" "$tmp/cf_new"
"$tmp/cf_new" "$src" "$tmp/chk.qbe" "$tmp/chk.floor.s"
if ! cmp -s "$tmp/new.qbe" "$tmp/chk.qbe" || ! cmp -s "$tmp/new.floor.s" "$tmp/chk.floor.s"; then
	echo "reseed: FIXPOINT FAILED — the new seed does not reproduce itself; committed seed left unchanged" >&2
	exit 1
fi

# 4. Commit the verified seed.
cp "$tmp/new.qbe" "$seed/cf.qbe"
cp "$tmp/new.floor.s" "$seed/floor.s"
echo "reseed: ok — seed regenerated and fixpoint verified -> $seed"
