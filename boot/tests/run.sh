#!/bin/sh
# Regression for cf0 — the self-hosted compiler.
#
# Every corpus test listed in `manifest` is compiled by cf0 (via driver.sh: cf0 -> qbe ->
# cc) and run; its exit code must match the test's `# expect:` directive. cfcc (the genesis
# tool) is gone, so this is no longer a differential check against a second compiler — cf0
# is validated directly against the corpus's expectations, and its self-reproduction is
# guaranteed separately by the seed fixpoint (boot/src/reseed.sh).
#
# The `manifest` is the cf0-supported SUBSET of the corpus (boot/tests/corpus/, ~1066
# tests total). As cf0's front end grows, more corpus tests join the manifest.
#
# Directives (read from the corpus `.cf`):
#   # expect: exit <n>   compile + run, exit <n>
#   # expect: error      must FAIL to compile
#   # args: <words>      argv for a run test
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
corpus="$here/corpus"
driver="$root/boot/src/driver.sh"

work=$(mktemp -d "${TMPDIR:-/tmp}/cf0-tests.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT

# Build cf0 from the seed so the suite always exercises the current compiler.
if ! "$root/boot/src/build.sh" >/dev/null; then
	echo "run: cf0 build failed" >&2
	exit 1
fi

pass=0
fail=0
while IFS= read -r name; do
	# Skip blanks and comments in the manifest.
	case "$name" in
	''|\#*) continue ;;
	esac

	cf="$corpus/$name"
	if [ -d "$cf" ]; then
		cf="$corpus/$name/main.cf"
	fi
	if [ ! -f "$cf" ]; then
		echo "FAIL $name (no such corpus test)"
		fail=$((fail + 1))
		continue
	fi

	expect=$(sed -n 's/^# *expect: *//p' "$cf" | head -1)
	args=$(sed -n 's/^# *args: *//p' "$cf" | head -1)
	if [ -z "$expect" ]; then
		echo "FAIL $name (no \`# expect:\` directive)"
		fail=$((fail + 1))
		continue
	fi

	bin="$work/${name%.cf}"

	if [ "$expect" = "error" ]; then
		# The program must FAIL to compile (cf0 rejects it, or the qbe/cc tail fails).
		if "$driver" "$cf" "$bin" >/dev/null 2>&1; then
			echo "FAIL $name (compiled, expected an error)"
			fail=$((fail + 1))
		else
			echo "ok   $name (rejected)"
			pass=$((pass + 1))
		fi
		continue
	fi

	want=${expect#exit }

	if ! "$driver" "$cf" "$bin" >/dev/null 2>&1; then
		echo "FAIL $name (cf0 did not compile)"
		fail=$((fail + 1))
		continue
	fi

	# shellcheck disable=SC2086
	perl -e 'alarm 10; exec @ARGV' "$bin" $args >/dev/null 2>&1
	code=$?

	if [ "$code" = "$want" ]; then
		echo "ok   $name (exit $code)"
		pass=$((pass + 1))
	else
		echo "FAIL $name (cf0 exit $code, expected $want)"
		fail=$((fail + 1))
	fi
done < "$here/manifest"

echo "---"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
