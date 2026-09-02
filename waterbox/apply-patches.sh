#!/bin/sh
# Applies the numbered patches to the extern/rpcs3 submodule. Idempotent:
# a tree that already carries the changes is left alone; anything else is an
# error worth seeing. The driver lives in waterbox/ and is built OUTSIDE the
# rpcs3 tree, so nothing is copied in.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
rpcs3="$root/extern/rpcs3"

for p in "$root"/patches/*.patch; do
	if git -C "$rpcs3" apply --check "$p" 2>/dev/null; then
		git -C "$rpcs3" apply "$p"
		echo "applied: $(basename "$p")"
	elif git -C "$rpcs3" apply --reverse --check "$p" 2>/dev/null; then
		echo "already applied: $(basename "$p")"
	else
		echo "NEITHER applies nor reverses: $(basename "$p")" >&2
		exit 1
	fi
done
