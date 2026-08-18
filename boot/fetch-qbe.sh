#!/bin/sh
# Fetch and build the vendored QBE backend into opt/qbe.
#
# opt/ is gitignored (FHS-style: third-party add-ons are fetched, not committed),
# so QBE is pinned here by exact commit SHA rather than living in the repo. The
# pin is the trust anchor: the seed subset's backend tail is `qbe` + `cc`, and
# pinning the commit keeps that tail reproducible from a known source.
#
# Re-running is idempotent: it re-verifies the pin and rebuilds only if needed.
set -eu

# c9x.me serves git:// (smart transport); dumb HTTP mangles object fetches. The
# transport is unauthenticated, but the QBE_COMMIT pin below is verified after
# checkout, so in-transit tampering is caught before anything is built.
QBE_REPO="git://c9x.me/qbe.git"
QBE_TAG="v1.3"
QBE_COMMIT="c0818978acec60ebb6167fade60fb7012cbf20ca"

root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/opt/qbe"

echo "fetch-qbe: pinning QBE $QBE_TAG ($QBE_COMMIT)"

# Already at the pin with a built binary → nothing to do (works offline).
if [ -x "$dest/qbe" ] && [ -d "$dest/.git" ] &&
   [ "$(git -C "$dest" rev-parse HEAD 2>/dev/null)" = "$QBE_COMMIT" ]; then
	echo "fetch-qbe: up to date -> $dest/qbe"
	exit 0
fi

if [ ! -d "$dest/.git" ]; then
	rm -rf "$dest"
	mkdir -p "$dest"
	git -C "$dest" init -q
	git -C "$dest" remote add origin "$QBE_REPO"
fi

# Fetch the pinned tag and hard-checkout its commit (full fetch; QBE is tiny).
git -C "$dest" fetch -q origin "refs/tags/$QBE_TAG"
git -C "$dest" checkout -q -f FETCH_HEAD

got=$(git -C "$dest" rev-parse HEAD)
if [ "$got" != "$QBE_COMMIT" ]; then
	echo "fetch-qbe: PIN MISMATCH" >&2
	echo "  expected $QBE_COMMIT" >&2
	echo "  got      $got" >&2
	echo "  tag $QBE_TAG at $QBE_REPO moved or was tampered with; refusing to build." >&2
	exit 1
fi

echo "fetch-qbe: building qbe"
make -C "$dest" -s

# Load-bearing smoke test: fail loudly if the build produced no runnable qbe.
if ! "$dest/qbe" -h >/dev/null 2>&1; then
	echo "fetch-qbe: build did not produce a runnable qbe at $dest/qbe" >&2
	exit 1
fi

echo "fetch-qbe: ok -> $dest/qbe"
