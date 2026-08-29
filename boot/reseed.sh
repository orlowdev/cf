#!/bin/sh
# Regenerate the committed seed after editing cf's `.cf` source, and VERIFY the fixpoint.
#
# The self-hosting maintenance loop (seed_subset §3): cf's binary is built from the seed
# (`boot/seed_mac/` + `boot/seed_linux/`), so a source edit leaves the seeds stale. This rebuilds cf from the OLD
# seed, has THAT cf recompile the (new) source into a fresh seed, then confirms the fresh
# seed is a true fixpoint — a cf built from it recompiles the source to the SAME thing.
# Only then is the new seed written. A non-fixpoint (the emitted IL doesn't reproduce)
# aborts without touching the committed seed.
#
# Note the bootstrap constraint: the OLD cf must already be able to compile the NEW source
# (you can only add a language feature cf already accepts). A feature that changes cf's
# own accepted subset needs a transitional two-step reseed.
#
# TWO RESEED MODES (the second is for self-affecting emit changes):
#
#   default (strict, commit stage-1, check stage-1 == stage-2)
#     stage-1 = cf_old(src)   — OLD binary's emit logic over the new source
#     stage-2 = cf_new(src)   — cf built FROM stage-1, its emit logic over the new source
#     require stage-1 == stage-2, commit stage-1.
#     This only converges when the OLD and NEW emit logic produce byte-identical output
#     for the new source — i.e. pure front-end / analysis additions that do not change
#     what cf emits for its OWN functions.
#
#   --transitional (commit stage-2, check stage-2 == stage-3)
#     For a SELF-AFFECTING change — one that alters the QBE/floor bytes cf emits for its
#     own source (the elastic floor, the %node ABI flip, any codegen change). Here the OLD
#     logic (stage-1) legitimately differs from the NEW logic (stage-2), so the strict
#     check can never pass. Instead build a third generation and require the NEW logic to
#     reproduce ITSELF (stage-2 == stage-3), then commit stage-2 — the standard GCC-style
#     bootstrap. Use this for one reseed across the transition, then revert to strict.
set -eu

transitional=0
if [ "${1:-}" = "--transitional" ]; then
	transitional=1
	shift
fi

root=$(cd "$(dirname "$0")/.." && pwd)
qbedir="$root/boot/vendor/qbe"
qbe="$qbedir/qbe"
seed="$root/boot/seed_mac"
lseed="$root/boot/seed_linux"
src="$root/boot/src/cf.cf"

# Build the vendored QBE (standalone qbe + embeddable object set) and gather the objects linked into
# each generation of cf (all but qbe's own main.o). Same embed as build.sh — cf self-contains QBE.
make -C "$qbedir" -s
embed=$(ls "$qbedir"/*.o "$qbedir"/*/*.o | grep -v '/main\.o$')

asm() { # <in.qbe> <in.floor.s> <out-bin>
	"$qbe" -t arm64_apple -o "$3.prog.s" "$1"
	cc -std=c99 -I "$qbedir" -c "$root/boot/qbe_embed.c" -o "$3.qbe_embed.o"
	# shellcheck disable=SC2086
	cc -nostdlib -lSystem -Wl,-e,_start -o "$3" "$2" "$3.prog.s" $embed "$3.qbe_embed.o"
}

tmp=$(mktemp -d "${TMPDIR:-/tmp}/cf-reseed.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

# `--skip-embeds`: the SEED is a std-LESS compiler (it reads lib/std from disk, like any dev build).
# The baked-in stdlib belongs only in the shipped `var/cf` (built by build.sh WITHOUT this flag), NOT
# in the committed trust-seed — so an `embed`/`embed_dir` never bloats the seed and a stdlib edit that
# leaves the compiler's own code untouched needs no reseed at all.

# Regenerate the LINUX seed by cross-emitting from the just-verified darwin compiler `$1`. The linux
# fixpoint is verified natively by CI (reseed runs on darwin and cannot build/run a linux cf), but the
# emit is deterministic and target-driven, so this compiler's linux emit is exactly what a linux-native
# cf reproduces. The two linux arches share one IL (generic ABI) and differ only in the floor.
regen_linux() { # <verified-darwin-cf>
	mkdir -p "$lseed"
	"$1" --skip-embeds --target linux-arm64   "$src" "$lseed/cf.qbe" "$lseed/floor.arm64.s"
	"$1" --skip-embeds --target linux-riscv64 "$src" "$tmp/lr.qbe"   "$lseed/floor.riscv64.s"
	if ! cmp -s "$lseed/cf.qbe" "$tmp/lr.qbe"; then
		echo "reseed: FIXPOINT FAILED — linux arm64/riscv64 IL diverged (must be identical, generic ABI)" >&2
		exit 1
	fi
	echo "reseed: ok — linux seed regenerated (native fixpoint verified in CI) -> $lseed"
}

# Regenerate the darwin-amd64 floor from the just-verified darwin compiler `$1`. darwin arm64 and amd64
# emit BYTE-IDENTICAL IL — they share BSD syscall NUMBERS, and the amd64 `syscall` class bit lives in
# the floor trampoline, not the IL — so amd64 REUSES the committed mac `cf.qbe`; we assert that identity
# here and commit only the new `floor.amd64.s`. The amd64 fixpoint is verified natively under Rosetta 2
# / CI (reseed runs on arm64 and cannot itself run an amd64 cf), but the emit is deterministic and
# target-driven, so this compiler's amd64 emit is exactly what an amd64-native cf reproduces.
regen_darwin_amd64() { # <verified-darwin-cf>
	"$1" --skip-embeds --target darwin-amd64 "$src" "$tmp/da.qbe" "$seed/floor.amd64.s"
	if ! cmp -s "$tmp/da.qbe" "$seed/cf.qbe"; then
		echo "reseed: FIXPOINT FAILED — darwin-amd64 IL diverged from the mac seed (must be identical)" >&2
		exit 1
	fi
	echo "reseed: ok — darwin-amd64 floor regenerated, IL reuses the mac seed -> $seed/floor.amd64.s"
}

# 1. cf₀ from the CURRENT (committed) seed.
asm "$seed/cf.qbe" "$seed/floor.arm64.s" "$tmp/cf_old"

# 2. cf₀ recompiles the (possibly edited) source → stage-1.
"$tmp/cf_old" --skip-embeds "$src" "$tmp/new.qbe" "$tmp/new.floor.s"

# 3. Build cf₁ from stage-1 and recompile → stage-2.
asm "$tmp/new.qbe" "$tmp/new.floor.s" "$tmp/cf_new"
"$tmp/cf_new" --skip-embeds "$src" "$tmp/chk.qbe" "$tmp/chk.floor.s"

if [ "$transitional" -eq 0 ]; then
	# STRICT: stage-1 == stage-2, commit stage-1.
	if ! cmp -s "$tmp/new.qbe" "$tmp/chk.qbe" || ! cmp -s "$tmp/new.floor.s" "$tmp/chk.floor.s"; then
		echo "reseed: FIXPOINT FAILED — the new seed does not reproduce itself; committed seed left unchanged" >&2
		echo "        (a self-affecting codegen change needs: boot/reseed.sh --transitional)" >&2
		exit 1
	fi
	cp "$tmp/new.qbe" "$seed/cf.qbe"
	cp "$tmp/new.floor.s" "$seed/floor.arm64.s"
	echo "reseed: ok — mac seed regenerated and fixpoint verified -> $seed"
	regen_darwin_amd64 "$tmp/cf_new"
	regen_linux "$tmp/cf_new"
	exit 0
fi

# TRANSITIONAL: build cf₂ from stage-2 and recompile → stage-3; require stage-2 == stage-3
# (the NEW emit logic reproduces itself), then commit stage-2.
asm "$tmp/chk.qbe" "$tmp/chk.floor.s" "$tmp/cf_newer"
"$tmp/cf_newer" --skip-embeds "$src" "$tmp/chk2.qbe" "$tmp/chk2.floor.s"
if ! cmp -s "$tmp/chk.qbe" "$tmp/chk2.qbe" || ! cmp -s "$tmp/chk.floor.s" "$tmp/chk2.floor.s"; then
	echo "reseed: FIXPOINT FAILED (transitional) — stage-2 does not reproduce itself; committed seed left unchanged" >&2
	exit 1
fi
cp "$tmp/chk.qbe" "$seed/cf.qbe"
cp "$tmp/chk.floor.s" "$seed/floor.arm64.s"
echo "reseed: ok — mac seed regenerated (transitional stage-2) and fixpoint verified -> $seed"
regen_darwin_amd64 "$tmp/cf_newer"
regen_linux "$tmp/cf_newer"
