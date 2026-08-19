#!/bin/sh
# Assemble a cf-compiled program end to end.
#
# cf cannot yet spawn `qbe`/`cc` itself (process-spawn is deferred to this
# external driver — seed_subset §M6), so it stops at emitting two text artifacts:
# the QBE IL (`prog.qbe`, the user code) and the arm64 asm floor (`floor.s`,
# `_start`, emitted verbatim since QBE has no `svc`). This driver runs the
# `qbe`→`cc` tail: qbe lowers the IL to assembly, then `cc -nostdlib -lSystem
# -Wl,-e,_start` links the freestanding binary (darwin needs -lSystem present for
# the loader even with no libc symbol used). This is the same tail boot/build.sh
# uses to assemble the seed into cf itself.
#
#   driver.sh <input.cf> <out-binary>
#
# The `!` allocation algebra and the root-page rule are validated unconditionally by cf on every
# compile, so there are no flags to forward.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
cf="$root/var/cf"
qbe="$root/opt/qbe/qbe"

if [ "$#" -ne 2 ]; then
	echo "usage: driver.sh <input.cf> <out-binary>" >&2
	exit 2
fi
in=$1
out=$2

tmp=$(mktemp -d "${TMPDIR:-/tmp}/cf-driver.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

"$cf" "$in" "$tmp/prog.qbe" "$tmp/floor.s"
"$qbe" -t arm64_apple -o "$tmp/prog.s" "$tmp/prog.qbe"
cc -nostdlib -lSystem -Wl,-e,_start -o "$out" "$tmp/floor.s" "$tmp/prog.s"
