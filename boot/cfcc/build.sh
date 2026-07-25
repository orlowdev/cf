#!/bin/sh
# Build the cfcc genesis compiler.
#
# cfcc is a hosted C program (it drives the vendored `qbe` and `cc`), so it is
# compiled by the system cc into var/cfcc — var/ is gitignored build data. The
# vendored qbe path is baked in so cfcc works from any working directory; run
# boot/fetch-qbe.sh first to populate it.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
qbe="$root/opt/qbe/qbe"
out="$root/var/cfcc"

if [ ! -x "$qbe" ]; then
	echo "build: vendored qbe missing at $qbe" >&2
	echo "       run boot/fetch-qbe.sh first" >&2
	exit 1
fi

mkdir -p "$root/var"
# -Wno-missing-field-initializers: the `Type`/`Param` structs are built with positional
# initializers that intentionally omit trailing fields (e.g. the fixed-width `bits`/`is_signed`,
# meaningful only for TY_FIXED); C zero-fills the rest, which is exactly what we want, so this
# -Wextra sub-warning is pure noise here (it never flags uninitialized memory).
cc -std=c11 -O2 -Wall -Wextra -Wno-missing-field-initializers \
	-DCF_QBE="\"$qbe\"" \
	-o "$out" \
	"$root/boot/cfcc/src/cfcc.c"

echo "build: ok -> $out"
