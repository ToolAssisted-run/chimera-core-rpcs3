#!/bin/sh
# The core gate. Every leg prints PASS/FAIL/SKIP with what it proved; the
# script exits non-zero if any leg failed. Legs arrive with their milestones:
#   native:deterministic   two native runs print identical RAM + TTY digests
#   sandbox:equivalent     core.wbx under miniBox prints the native run's lines
#   sandbox:rewind         save mid-run, finish, load, finish again: equal
#   sandbox:rerecord       save+load around every frame changes nothing
# Run from anywhere; artifacts land in waterbox/work/gate.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
work="$here/work/gate"
rom="$root/tests/roms/lv2test.elf"
frames="${FRAMES:-600}"
native="$here/obj-native/run-native"
fail=0

pass() { echo "PASS: $*"; }
failed() { echo "FAIL: $*"; fail=1; }
skip() { echo "SKIP: $*"; }

rm -rf "$work"
mkdir -p "$work"

if [ ! -x "$native" ]; then
	failed "native runner missing (build-native.sh, then make -f native.mk)"
	exit 1
fi
if [ ! -f "$rom" ]; then
	python3 "$root/tests/asm/ppc.py" "$root/tests/asm/lv2test.s" "$rom" >/dev/null || failed "lv2test.elf does not assemble"
fi

# ---- native:deterministic ---------------------------------------------
for r in a b; do
	"$native" --work "$work/$r" --frames "$frames" --report 50 --tty-out "$work/tty-$r.txt" "$rom" 2>"$work/$r.err" | grep '^frame' > "$work/run-$r.txt"
done
if [ ! -s "$work/run-a.txt" ]; then
	failed "native run produced no frames ($(tail -1 "$work/a.err"))"
elif cmp -s "$work/run-a.txt" "$work/run-b.txt" && cmp -s "$work/tty-a.txt" "$work/tty-b.txt"; then
	pass "native deterministic at $frames frames ($(wc -c < "$work/tty-a.txt") TTY bytes, $(tail -1 "$work/run-a.txt" | sed 's/.*threads/threads/'))"
else
	failed "native runs differ (diff $work/run-a.txt $work/run-b.txt)"
fi

# ---- the sandbox legs -------------------------------------------------
wbx="$here/bin/run-wbx"
core="$here/bin/core.wbx"
if [ ! -x "$wbx" ] || [ ! -f "$core" ]; then
	skip "sandbox legs: build-core.sh has not produced bin/core.wbx + bin/run-wbx"
	exit $fail
fi

"$wbx" "$core" --frames "$frames" --report 50 --tty-out "$work/tty-wbx.txt" "$rom" 2>"$work/wbx.err" | grep '^frame' > "$work/run-wbx.txt"
if [ ! -s "$work/run-wbx.txt" ]; then
	failed "sandbox run produced no frames ($(tail -1 "$work/wbx.err"))"
elif cmp -s "$work/run-a.txt" "$work/run-wbx.txt" && cmp -s "$work/tty-a.txt" "$work/tty-wbx.txt"; then
	pass "native == sandbox at $frames frames (RAM + TTY digests)"
else
	failed "sandbox differs from native (diff $work/run-a.txt $work/run-wbx.txt)"
fi

if "$wbx" "$core" --frames "$frames" --rewind "$rom" > "$work/rewind.txt" 2>"$work/rewind.err"; then
	pass "rewind leg - $(grep '^rewind' "$work/rewind.txt")"
else
	failed "rewind leg - $(grep '^rewind' "$work/rewind.txt" || tail -1 "$work/rewind.err")"
fi

"$wbx" "$core" --frames "$((frames / 3))" --report 50 --rerecord "$rom" 2>"$work/rerecord.err" | grep '^frame' > "$work/rerecord.txt"
"$wbx" "$core" --frames "$((frames / 3))" --report 50 "$rom" 2>/dev/null | grep '^frame' > "$work/plain.txt"
if [ -s "$work/rerecord.txt" ] && cmp -s "$work/rerecord.txt" "$work/plain.txt"; then
	pass "rerecord leg - save+load around every frame changes nothing ($((frames / 3)) frames)"
else
	failed "rerecord leg - $(tail -1 "$work/rerecord.err")"
fi

exit $fail
