#!/bin/sh
# cf installer — download a released `cf` binary for your platform and drop it on your PATH.
#
#   curl -fsSL https://raw.githubusercontent.com/orlowdev/cf/margarita/install.sh | sh
#
# Yes, you are about to pipe a script from the internet straight into a shell. In the grand tradition
# of "install everything this way and hope for the best", we at least try to earn a sliver of that
# trust: this script pins nothing you can't see, downloads only from github.com/orlowdev/cf's own
# releases, and VERIFIES the binary against the sha256 published beside it before it touches your disk.
# You are still encouraged to read it first (you're reading it now — good). No `sudo`, no daemons, no
# telemetry; it copies one static binary and stops.
#
# Knobs (env vars):
#   CF_VERSION=<tag>        install this exact release tag (e.g. 1.23.61-stable). Overrides CF_CHANNEL.
#   CF_CHANNEL=<ring>       nightly | rc | latest | stable — newest release on that ring. Default: the
#                           newest release of any ring that has a build for your platform.
#   CF_INSTALL_DIR=<dir>    where to put the `cf` binary. Default: $HOME/.local/bin.
#   GITHUB_TOKEN=<token>    optional — used only to raise the GitHub API rate limit (60/hr anonymous).
set -eu

REPO="orlowdev/cf"
BIN_NAME="cf"

# --- pretty output (falls back to plain when not a tty) -----------------------------------------
if [ -t 2 ]; then
	B="$(printf '\033[1m')"; DIM="$(printf '\033[2m')"; RED="$(printf '\033[31m')"
	GRN="$(printf '\033[32m')"; YLW="$(printf '\033[33m')"; RST="$(printf '\033[0m')"
else
	B=""; DIM=""; RED=""; GRN=""; YLW=""; RST=""
fi
say()  { printf '%s\n' "$*" >&2; }
info() { printf '%s==>%s %s\n' "$B" "$RST" "$*" >&2; }
warn() { printf '%swarning:%s %s\n' "$YLW" "$RST" "$*" >&2; }
die()  { printf '%serror:%s %s\n' "$RED" "$RST" "$*" >&2; exit 1; }

# --- fetch helper (curl or wget) ----------------------------------------------------------------
if command -v curl >/dev/null 2>&1; then
	fetch()      { curl -fsSL ${GITHUB_TOKEN:+-H "Authorization: Bearer $GITHUB_TOKEN"} "$1"; }
	download()   { curl -fsSL --retry 3 -o "$2" "$1"; }
elif command -v wget >/dev/null 2>&1; then
	fetch()      { wget -qO- ${GITHUB_TOKEN:+--header="Authorization: Bearer $GITHUB_TOKEN"} "$1"; }
	download()   { wget -q --tries=3 -O "$2" "$1"; }
else
	die "need either curl or wget on PATH"
fi

# --- detect platform ----------------------------------------------------------------------------
os="$(uname -s)"; arch="$(uname -m)"
case "$os" in
	Darwin) os="darwin" ;;
	Linux)  os="linux" ;;
	*) die "unsupported OS '$os' (cf ships darwin and linux builds)" ;;
esac
case "$arch" in
	arm64 | aarch64) arch="arm64" ;;
	x86_64 | amd64)  arch="amd64" ;;
	riscv64)         arch="riscv64" ;;
	*) die "unsupported architecture '$arch'" ;;
esac
plat="${os}-${arch}"
case "$plat" in
	darwin-arm64 | darwin-amd64 | linux-arm64 | linux-amd64 | linux-riscv64) ;;
	*) die "no cf build for '$plat' (built: darwin-arm64/amd64, linux-arm64/amd64/riscv64)" ;;
esac
info "platform: ${B}${plat}${RST}"

# --- resolve the release asset ------------------------------------------------------------------
# Assets are named `cf-<tag>-<plat>` (+ a `.sha256` sibling). We pull the release list (newest first)
# and pick the first download URL that matches the requested version/channel for this platform.
api="https://api.github.com/repos/${REPO}/releases?per_page=50"
info "querying releases ${DIM}(${REPO})${RST}"
json="$(fetch "$api")" || die "could not reach the GitHub API"

urls="$(printf '%s\n' "$json" | grep -o '"browser_download_url"[[:space:]]*:[[:space:]]*"[^"]*"' \
	| sed 's/.*"\(https[^"]*\)"$/\1/')"
[ -n "$urls" ] || die "no release assets found for ${REPO} (has a release been cut yet?)"

if [ -n "${CF_VERSION:-}" ]; then
	pat="/cf-${CF_VERSION}-${plat}$"
	what="version ${CF_VERSION}"
elif [ -n "${CF_CHANNEL:-}" ]; then
	pat="/cf-.*-${CF_CHANNEL}-${plat}$"
	what="channel ${CF_CHANNEL}"
else
	pat="/cf-.*-${plat}$"
	what="latest available"
fi

asset_url="$(printf '%s\n' "$urls" | grep -E "$pat" | head -1 || true)"
[ -n "$asset_url" ] || die "no build for ${plat} matching ${what}. Try a different CF_CHANNEL (nightly|rc|latest|stable) or CF_VERSION."
asset="$(basename "$asset_url")"
tag="$(printf '%s' "$asset" | sed "s/^cf-\\(.*\\)-${plat}$/\\1/")"
info "selected: ${B}${asset}${RST} ${DIM}(tag ${tag})${RST}"

# --- download + verify --------------------------------------------------------------------------
tmp="$(mktemp -d "${TMPDIR:-/tmp}/cf-install.XXXXXX")" || die "mktemp failed"
trap 'rm -rf "$tmp"' EXIT INT TERM

info "downloading"
download "$asset_url" "$tmp/$BIN_NAME" || die "download failed: $asset_url"

if sum_url_ok="$(printf '%s\n' "$urls" | grep -F "${asset}.sha256" | head -1)"; [ -n "$sum_url_ok" ]; then
	info "verifying sha256"
	download "$sum_url_ok" "$tmp/sum" || die "could not fetch the checksum"
	want="$(awk '{print $1; exit}' "$tmp/sum")"
	if command -v sha256sum >/dev/null 2>&1; then
		got="$(sha256sum "$tmp/$BIN_NAME" | awk '{print $1}')"
	elif command -v shasum >/dev/null 2>&1; then
		got="$(shasum -a 256 "$tmp/$BIN_NAME" | awk '{print $1}')"
	else
		got=""; warn "no sha256sum/shasum found — skipping checksum verification"
	fi
	if [ -n "$got" ] && [ "$got" != "$want" ]; then
		die "checksum MISMATCH — refusing to install.\n  want $want\n  got  $got"
	fi
	[ -n "$got" ] && info "checksum ${GRN}ok${RST}"
else
	warn "no published checksum for this asset — installing unverified"
fi

chmod +x "$tmp/$BIN_NAME"

# --- install ------------------------------------------------------------------------------------
dest="${CF_INSTALL_DIR:-$HOME/.local/bin}"
mkdir -p "$dest" || die "cannot create install dir: $dest"
target="$dest/$BIN_NAME"
if ! mv "$tmp/$BIN_NAME" "$target" 2>/dev/null; then
	# cross-device (e.g. tmp on another fs): copy then remove.
	cp "$tmp/$BIN_NAME" "$target" || die "cannot write $target"
fi
info "installed ${B}${target}${RST}"

# --- sanity check + PATH guidance ---------------------------------------------------------------
# The downloaded binary is for THIS host, so it should run (unless riscv fetched on a non-riscv box).
if [ "$plat" != "linux-riscv64" ] || [ "$arch" = "riscv64" ]; then
	if v="$("$target" --version 2>/dev/null)"; then
		info "cf ${GRN}${v}${RST} is ready"
	else
		warn "the binary did not run cleanly here — is $plat really your platform?"
	fi
fi

case ":$PATH:" in
	*":$dest:"*) : ;;
	*)
		say ""
		warn "$dest is not on your PATH. Add it, e.g.:"
		say "    ${B}export PATH=\"$dest:\$PATH\"${RST}   ${DIM}# add to your ~/.profile, ~/.bashrc or ~/.zshrc${RST}"
		;;
esac

say ""
info "done. Try: ${B}${BIN_NAME} --version${RST}  or  ${B}${BIN_NAME} your_program.cf -o your_program${RST}"
