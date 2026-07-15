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
cc -std=c11 -O2 -Wall -Wextra \
	-DCF_QBE="\"$qbe\"" \
	-o "$out" \
	"$root/boot/cfcc/src/cfcc.c"

echo "build: ok -> $out"
