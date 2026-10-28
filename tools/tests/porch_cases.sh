#!/bin/sh
# porch_cases.sh <cob_interp> <popcorn_comp> <cc> -- awkward console input must
# behave identically in the interpreter and in a compiled program (both call
# the same libporch, so even the warnings match byte for byte).
INTERP=$1; COMP=$2; CC=$3
[ -n "$CC" ] && [ -x "$COMP" ] || { echo "no C compiler -- skipped"; exit 0; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
cp "$(dirname "$0")/porch_input.cob" porch_input.cob
"$INTERP" porch_input.cob < /dev/null > /dev/null 2>&1          # writes porch_input.strawberry
"$COMP" porch_input.strawberry -o native --cc "$CC" > /dev/null 2>&1 || { echo "compile failed"; exit 1; }
fail=0
check() {
    "$INTERP" porch_input.cob --no-cache < "$2" > i.out 2> i.err
    ./native < "$2" > n.out 2> n.err
    if cmp -s i.out n.out && cmp -s i.err n.err; then echo "ok    $1"; else echo "FAIL  $1"; diff i.out n.out; diff i.err n.err; fail=1; fi
}
printf 'Ada\n41\nfree text\n3\n10\n20\n30\n'     > a; check "normal answers" a
printf 'Bob\r\nabc\r\n\r\n0\r\nyes\r\n'          > b; check "CRLF, blank and non-numeric" b
: > c;                                              check "end of input at once" c
printf 'x\n  7  \nline\n2\n5\n6\nmore'           > d; check "padded number, no final newline" d
printf 'Z\n12abc\n\n1\n-3\n'                     > e; check "trailing junk and negative" e
# the warning text itself
"$INTERP" porch_input.cob --no-cache < b 2>&1 >/dev/null | grep -q '\[porch\] warning: porch_ask_int() got non-numeric input "abc"; treated as 0' || { echo "FAIL  warning text"; fail=1; }
exit $fail
