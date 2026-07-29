#!/bin/sh
# Assemble a cf0-compiled program end to end.
#
# cf0 cannot yet spawn `qbe`/`cc` itself (process-spawn is deferred to this
# external driver — seed_subset §M6), so it stops at emitting two text artifacts:
# the QBE IL (`prog.qbe`, the user code) and the arm64 asm floor (`floor.s`,
# `_start`, emitted verbatim since QBE has no `svc`). This driver runs the
# `qbe`→`cc` tail the way cfcc does internally: qbe lowers the IL to assembly,
# then `cc -nostdlib -lSystem -Wl,-e,_start` links the freestanding binary
# (darwin needs -lSystem present for the loader even with no libc symbol used).
#
#   driver.sh <input.cf> <out-binary>
set -eu

root=$(cd "$(dirname "$0")/../.." && pwd)
cf0="$root/var/cf0"
qbe="$root/opt/qbe/qbe"

if [ "$#" -ne 2 ]; then
	echo "usage: driver.sh <input.cf> <out-binary>" >&2
	exit 2
fi
in=$1
out=$2

tmp=$(mktemp -d "${TMPDIR:-/tmp}/cf0-driver.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

"$cf0" "$in" "$tmp/prog.qbe" "$tmp/floor.s"
"$qbe" -t arm64_apple -o "$tmp/prog.s" "$tmp/prog.qbe"
cc -nostdlib -lSystem -Wl,-e,_start -o "$out" "$tmp/floor.s" "$tmp/prog.s"
