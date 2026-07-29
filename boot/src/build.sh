#!/bin/sh
# Build cf0 — the self-hosted C! compiler (the keeper) — from its `.cf` source
# using the cfcc genesis tool. cfcc flattens the imported modules (cli, token,
# lexer, parser, emit, io) into the one program rooted at cf0.cf. Output is
# var/cf0 (var/ is gitignored build data).
#
# Requires var/cfcc — run boot/cfcc/build.sh first.
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
cfcc="$root/var/cfcc"
out="$root/var/cf0"

if [ ! -x "$cfcc" ]; then
	echo "build: cfcc missing at $cfcc" >&2
	echo "       run boot/cfcc/build.sh first" >&2
	exit 1
fi

"$cfcc" c "$root/boot/src/cf0.cf" -o "$out"
echo "build: ok -> $out"
