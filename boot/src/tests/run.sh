#!/bin/sh
# Differential regression for cf0.
#
# Every corpus test listed in `manifest` is compiled by BOTH the cfcc genesis
# tool and cf0 (the self-hosted compiler, via driver.sh). Their runtime exit
# codes must AGREE with each other AND match the test's `# expect:` directive.
# This is the self-hosting validation: cf0 reuses the cfcc corpus rather than a
# parallel test set, so any divergence between the two compilers surfaces here.
# As cf0's front end grows, tests move into the manifest from boot/cfcc/tests/.
#
# Directives (read from the corpus `.cf`, same as the cfcc runner):
#   # expect: exit <n>   compile with both, run both, both exit <n>
#   # expect: error      both must FAIL to compile
#   # args: <words>      argv for a run test
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
cfcc="$root/var/cfcc"
corpus="$root/boot/cfcc/tests"
driver="$root/boot/src/driver.sh"

work=$(mktemp -d "${TMPDIR:-/tmp}/cf0-tests.XXXXXX") || exit 1
trap 'rm -rf "$work"' EXIT

# Build both compilers so the suite always exercises current source.
if ! "$root/boot/cfcc/build.sh" >/dev/null; then
	echo "run: cfcc build failed" >&2
	exit 1
fi
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

	abin="$work/a_${name%.cf}"
	bbin="$work/b_${name%.cf}"

	if [ "$expect" = "error" ]; then
		"$cfcc" c -o "$abin" "$cf" >/dev/null 2>&1
		a=$?
		"$driver" "$cf" "$bbin" >/dev/null 2>&1
		b=$?
		# Both must FAIL to compile (nonzero). (b nonzero covers a cf0 reject or a
		# qbe/cc failure — for a rejected program cf0 exits before emitting.)
		if [ "$a" -ne 0 ] && [ "$b" -ne 0 ]; then
			echo "ok   $name (both rejected)"
			pass=$((pass + 1))
		else
			echo "FAIL $name (cfcc rc=$a, cf0 rc=$b; both should reject)"
			fail=$((fail + 1))
		fi
		continue
	fi

	want=${expect#exit }

	if ! "$cfcc" c -o "$abin" "$cf" >/dev/null 2>&1; then
		echo "FAIL $name (cfcc did not compile)"
		fail=$((fail + 1))
		continue
	fi
	if ! "$driver" "$cf" "$bbin" >/dev/null 2>&1; then
		echo "FAIL $name (cf0 did not compile)"
		fail=$((fail + 1))
		continue
	fi

	# shellcheck disable=SC2086
	perl -e 'alarm 10; exec @ARGV' "$abin" $args >/dev/null 2>&1
	acode=$?
	# shellcheck disable=SC2086
	perl -e 'alarm 10; exec @ARGV' "$bbin" $args >/dev/null 2>&1
	bcode=$?

	if [ "$acode" = "$want" ] && [ "$bcode" = "$want" ]; then
		echo "ok   $name (exit $bcode, agrees with cfcc)"
		pass=$((pass + 1))
	else
		echo "FAIL $name (cfcc exit $acode, cf0 exit $bcode, expected $want)"
		fail=$((fail + 1))
	fi
done < "$here/manifest"

echo "---"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
