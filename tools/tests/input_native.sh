#!/bin/sh
# Checks that a native binary built by popcorn_comp reads input exactly like cob_interp.
# Usage (from the repo root, after `make cob_interp popcorn_comp`):
#   sh tools/tests/input_native.sh [--cc gcc]
set -eu
cd "$(dirname "$0")/../.."
CC_ARGS="${*:-}"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
cp tools/tests/input.cob "$T/input.cob"
./bin/cob_interp "$T/input.cob" < /dev/null > /dev/null 2>&1      # writes input.strawberry
# shellcheck disable=SC2086
./bin/popcorn_comp "$T/input.strawberry" -o "$T/input_native" $CC_ARGS > /dev/null
fail=0
check() {
  ./bin/cob_interp "$T/input.cob" --no-cache < "$2" > "$T/i.out" 2> "$T/i.err" || true
  "$T/input_native" < "$2" > "$T/n.out" 2> "$T/n.err" || true
  sed 's/\[cob_interp\]/[cob]/' "$T/i.err" > "$T/i.err2"
  if cmp -s "$T/i.out" "$T/n.out" && cmp -s "$T/i.err2" "$T/n.err"; then echo "ok    $1"; else echo "FAIL  $1"; fail=1; fi
}
printf 'Ada\n41\nfree text\n3\n10\n20\n30\n'            > "$T/a"; check "normal answers" "$T/a"
printf 'Bob\r\nabc\r\n\r\n0\r\nyes\r\n'                 > "$T/b"; check "CRLF, blank and non-numeric" "$T/b"
: > "$T/c";                                                        check "end of input at once" "$T/c"
printf 'x\n  7  \nline\n2\n5\n6\nmore'                  > "$T/d"; check "padded number, no final newline" "$T/d"
printf 'Z\n12abc\n\n1\n-3\n'                            > "$T/e"; check "trailing junk and negative" "$T/e"
exit $fail
