#!/bin/sh
# Regression for cf — the self-hosted compiler.
#
# Every corpus test listed in `manifest` is built by the self-contained cf (`cf <src> -o <bin>`:
# compile -> embedded QBE -> link) and run; its exit code must match the test's `# expect:`
# directive — the same path a user takes, so the suite exercises the shipped compiler end to end.
# cfcc (the genesis
# tool) is gone, so this is no longer a differential check against a second compiler — cf
# is validated directly against the corpus's expectations, and its self-reproduction is
# guaranteed separately by the seed fixpoint (boot/reseed.sh).
#
# The `manifest` is the cf-supported SUBSET of the corpus (boot/tests/corpus/, ~1066
# tests total). As cf's front end grows, more corpus tests join the manifest.
#
# Directives (read from the corpus `.cf`):
#   # expect: exit <n>   compile + run, exit <n>
#   # expect: error      must FAIL to compile
#   # args: <words>      argv for a run test
#   # cf-flags: <words>  extra flags forwarded to cf (e.g. `--target bare-arm64 --root-size 0`).
#                        A bare target emits an ELF the host can't run, so this is for `error` tests
#                        only (compile-rejection); cf fails before the qbe/link tail is reached.
#
# PARALLEL: tests run concurrently, one job per CPU by default (override with CF_TEST_JOBS;
# `CF_TEST_JOBS=1` restores the serial order). cf is built ONCE up front and only read after
# that, and every `cf` invocation writes to its own mktemp dir, so the jobs share nothing
# writable. Result lines stream unordered; each job also records its line in a per-test file,
# and the summary is counted from those (a test that vanishes without a result counts failed).
# The old caveat about running the suite in parallel still holds ACROSS runs, not within one:
# two suite instances clash over the shared `var/cf` build.
#
# ⚠ macOS scan tax: the FIRST exec of every freshly linked binary waits on the system's
# malware scan (syspolicyd) — ~1-2s per binary, serialized machine-wide, cached per file
# afterwards. Every run test here is a fresh binary, so the scan can dominate the suite and
# parallelism cannot hide it (the run phase stays scan-bound); the alarm below is sized so a
# binary queued behind a full scan pipeline is not misread as a hang. To remove the tax,
# grant your terminal the Developer Tools exemption (System Settings → Privacy & Security →
# Developer Tools, or `sudo spctl developer-mode enable-terminal` for Terminal.app) — then
# the suite is compile-bound and the parallel win is real.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
corpus="$here/tests/corpus"
cfbin="$root/var/cf"

work=$(mktemp -d "${TMPDIR:-/tmp}/cf-tests.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/res"

jobs=${CF_TEST_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}

# First-exec scan amortization: the scan is paid once PER FILE, and cf is deterministic —
# an unchanged test rebuilds byte-identical output. So run binaries out of a persistent
# cache: byte-identical rebuild → exec the already-scanned cached file (no scan); changed
# binary → replace the entry, pay the scan once. One entry per test, self-pruning.
# Disable with CF_TEST_BIN_CACHE="" (or point it elsewhere).
bincache=${CF_TEST_BIN_CACHE-$HOME/.cache/cf-tests-bin}
if [ -n "$bincache" ]; then
	mkdir -p "$bincache" || bincache=""
fi

# Build cf from the seed so the suite always exercises the current compiler.
if ! "$root/boot/build.sh" >/dev/null; then
	echo "run: cf build failed" >&2
	exit 1
fi

export corpus cfbin work bincache

# One test per invocation: $1 is the manifest name. The result line is echoed live (unordered
# under parallelism) and recorded in $work/res/<name> for the ordered count at the end.
run_one='
	name=$1
	res="$work/res/$(printf "%s" "$name" | tr "/" "_")"
	finish() {
		printf "%s\n" "$1" > "$res"
		printf "%s\n" "$1"
		exit 0
	}

	cf="$corpus/$name"
	if [ -d "$cf" ]; then
		cf="$corpus/$name/main.cf"
	fi
	if [ ! -f "$cf" ]; then
		finish "FAIL $name (no such corpus test)"
	fi

	expect=$(sed -n "s/^# *expect: *//p" "$cf" | head -1)
	args=$(sed -n "s/^# *args: *//p" "$cf" | head -1)
	cfflags=$(sed -n "s/^# *cf-flags: *//p" "$cf" | head -1)
	if [ -z "$expect" ]; then
		finish "FAIL $name (no \`# expect:\` directive)"
	fi

	bin="$work/${name%.cf}"

	if [ "$expect" = "error" ]; then
		# The program must FAIL to build (cf rejects it, or the embedded-QBE/link tail fails).
		# shellcheck disable=SC2086
		if "$cfbin" $cfflags "$cf" -o "$bin" >/dev/null 2>&1; then
			finish "FAIL $name (compiled, expected an error)"
		else
			finish "ok   $name (rejected)"
		fi
	fi

	want=${expect#exit }

	# shellcheck disable=SC2086
	if ! "$cfbin" $cfflags "$cf" -o "$bin" >/dev/null 2>&1; then
		finish "FAIL $name (cf did not compile)"
	fi

	# Swap in the already-scanned cached copy when the fresh build is byte-identical
	# (see the cache note above); otherwise the fresh binary replaces the entry.
	if [ -n "$bincache" ]; then
		cbin="$bincache/$(printf "%s" "${name%.cf}" | tr "/" "_")"
		if ! cmp -s "$bin" "$cbin" 2>/dev/null; then
			mv -f "$bin" "$cbin"
		fi
		bin="$cbin"
	fi

	# 60s: generous because a fresh binary may sit in the machine-wide first-exec scan queue
	# behind every other job (see the scan-tax note above) — a real hang still trips it.
	# shellcheck disable=SC2086
	perl -e "alarm 60; exec @ARGV" "$bin" $args >/dev/null 2>&1
	code=$?

	if [ "$code" = "$want" ]; then
		finish "ok   $name (exit $code)"
	else
		finish "FAIL $name (cf exit $code, expected $want)"
	fi
'

# Skip blanks and comments in the manifest, then fan the names out. The feed is
# NUL-delimited: plain xargs tokenizes quotes and apostrophes ("unterminated quote" aborts
# the whole stream mid-suite); -0 turns that off.
grep -v -e '^[[:space:]]*$' -e '^#' "$here/tests/manifest" \
	| tr '\n' '\0' \
	| xargs -0 -P "$jobs" -n 1 sh -c "$run_one" cf-test-one

pass=0
fail=0
while IFS= read -r name; do
	case "$name" in
	''|\#*) continue ;;
	esac
	res="$work/res/$(printf '%s' "$name" | tr '/' '_')"
	if [ ! -f "$res" ]; then
		echo "FAIL $name (no result — test job died)"
		fail=$((fail + 1))
	elif grep -q '^ok' "$res"; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
	fi
done < "$here/tests/manifest"

echo "---"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
