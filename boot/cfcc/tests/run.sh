#!/bin/sh
# Regression suite for cfcc.
#
# Compiles every tests/NNN_*.cf and checks it against a `# expect:` directive
# embedded in the file (cfcc strips it as an ordinary comment). Two forms:
#
#   # expect: exit <n>   must compile, then run (with `# args:` if present) and
#                        exit with code <n>
#   # expect: error      must FAIL to compile (cfcc exits non-zero)
#
# An optional `# args: <words>` line supplies argv to a run test.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
cfcc="$root/var/cfcc"
work=$(mktemp -d "${TMPDIR:-/tmp}/cfcc-tests.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT

# Build first so the suite always exercises the current source — a failed build
# must stop the run, not silently test a stale binary.
if ! "$root/boot/cfcc/build.sh" >/dev/null; then
	echo "run: cfcc build failed" >&2
	exit 1
fi

# Fail loudly on an empty corpus rather than reporting a false green.
first=$(printf '%s\n' "$here"/[0-9]*.cf | head -1)
if [ ! -e "$first" ]; then
	echo "run: no NNN_*.cf tests found in $here" >&2
	exit 1
fi

pass=0
fail=0
for cf in "$here"/[0-9]*.cf; do
	name=$(basename "$cf")
	expect=$(sed -n 's/^# *expect: *//p' "$cf" | head -1)
	args=$(sed -n 's/^# *args: *//p' "$cf" | head -1)
	out="$work/${name%.cf}"

	if [ -z "$expect" ]; then
		echo "FAIL $name (no \`# expect:\` directive)"
		fail=$((fail + 1))
		continue
	fi

	if [ "$expect" = "error" ]; then
		if "$cfcc" c -o "$out" "$cf" >/dev/null 2>&1; then
			echo "FAIL $name (expected a compile error, but it compiled)"
			fail=$((fail + 1))
		else
			echo "ok   $name (rejected as expected)"
			pass=$((pass + 1))
		fi
		continue
	fi

	want=${expect#exit }
	if ! "$cfcc" c -o "$out" "$cf" >/dev/null 2>&1; then
		echo "FAIL $name (did not compile; expected exit $want)"
		fail=$((fail + 1))
		continue
	fi
	# Word-split $args into argv on purpose.
	# shellcheck disable=SC2086
	"$out" $args
	got=$?
	if [ "$got" = "$want" ]; then
		echo "ok   $name (exit $got)"
		pass=$((pass + 1))
	else
		echo "FAIL $name (exit $got; expected $want)"
		fail=$((fail + 1))
	fi
done

echo "---"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
