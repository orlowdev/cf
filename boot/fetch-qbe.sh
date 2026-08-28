#!/bin/sh
# Update the vendored QBE pin. MAINTAINER TOOL — not part of the build.
#
# QBE is vendored as a git subtree at boot/vendor/qbe (committed source, hermetic
# offline builds; see boot/vendor/README.md). This script pulls a NEW pinned tag from
# upstream into that subtree. It touches the network (git://) only here, never at build
# time. After running, review the diff and update the pin recorded in boot/vendor/README.md.
#
#   sh boot/fetch-qbe.sh            # pull the pin below
#   QBE_TAG=v1.4 sh boot/fetch-qbe.sh   # pull a different tag
#
# c9x.me serves git:// (smart transport); the transport is unauthenticated, so verify the
# resulting commit against upstream before committing the update.
set -eu

QBE_REPO="git://c9x.me/qbe.git"
QBE_TAG="${QBE_TAG:-v1.3}"

root=$(cd "$(dirname "$0")/.." && pwd)
prefix="boot/vendor/qbe"

if [ ! -d "$root/$prefix" ]; then
	echo "fetch-qbe: $prefix missing — initial vendoring is a one-time:" >&2
	echo "  git subtree add --prefix=$prefix $QBE_REPO $QBE_TAG --squash" >&2
	exit 1
fi

echo "fetch-qbe: pulling QBE $QBE_TAG from $QBE_REPO into $prefix (squashed)"
cd "$root"
git subtree pull --prefix="$prefix" "$QBE_REPO" "$QBE_TAG" --squash \
	-m "Update vendored QBE to $QBE_TAG"

echo "fetch-qbe: done. Now:"
echo "  1. verify boot/vendor/qbe HEAD matches upstream $QBE_TAG"
echo "  2. update the pin in boot/vendor/README.md"
echo "  3. rebuild + reseed (boot/build.sh, boot/reseed.sh --transitional)"
