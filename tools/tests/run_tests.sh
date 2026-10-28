#!/bin/sh
# run_tests.sh -- Cob regression tests.
#
#   make test            (builds cob_interp, popcorn_comp and the stdlib first)
#   sh tools/tests/run_tests.sh [--no-native]
#
# Each tools/tests/NAME.cob with a NAME.expected is run through cob_interp
# (stdin from NAME.stdin if present) and, when a C compiler is available,
# compiled with popcorn_comp and run again; stdout must match NAME.expected
# both ways, and the two runs' stderr must match each other (modulo the
# "[cob_interp]" / "[cob]" prefix). repl_*.stdin files are fed to the REPL.
# errors.sh, cache.sh and static.sh are separate scripts.
#
# NOTE: tests must not be named after a stdlib module (porch.cob, pantry.cob
# ...): `shuck porch` would find the test itself first -- local files shadow
# the standard library on purpose.
cd "$(dirname "$0")/../.." || exit 1
ROOT=$(pwd)
INTERP="$ROOT/bin/cob_interp"
COMP="$ROOT/bin/popcorn_comp"
TESTS="$ROOT/tools/tests"
NATIVE=1
[ "$1" = "--no-native" ] && NATIVE=0

for need in "$INTERP" "$COMP" "$ROOT/bin/lib/libporch.so" ; do :; done
[ -x "$INTERP" ] || { echo "build first: make cob_interp popcorn_comp stdlib"; exit 1; }

CC_BIN=""
if [ "$NATIVE" = 1 ] && [ -x "$COMP" ]; then
    for c in gcc cc clang; do command -v "$c" >/dev/null 2>&1 && { CC_BIN=$c; break; }; done
fi

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok()  { pass=$((pass + 1)); echo "ok    $1"; }
bad() { fail=$((fail + 1)); echo "FAIL  $1"; [ -n "$2" ] && echo "      $2"; }

# ---- program tests: interpreter, then compiled
for src in "$TESTS"/*.cob; do
    name=$(basename "$src" .cob)
    exp="$TESTS/$name.expected"
    [ -f "$exp" ] || continue
    stdin_file=/dev/null; [ -f "$TESTS/$name.stdin" ] && stdin_file="$TESTS/$name.stdin"
    w="$T/$name"; mkdir -p "$w"; cp "$src" "$w/$name.cob"
    ( cd "$w" && "$INTERP" "$name.cob" --no-cache < "$stdin_file" > out.txt 2> err.txt )
    if cmp -s "$w/out.txt" "$exp"; then ok "interp   $name"
    else bad "interp   $name" "diff: $(diff "$w/out.txt" "$exp" | head -4 | tr '\n' '|')"; fi
    if [ -n "$CC_BIN" ]; then
        ( cd "$w" && "$INTERP" "$name.cob" < /dev/null > /dev/null 2>&1 )   # writes NAME.strawberry
        if ( cd "$w" && "$COMP" "$name.strawberry" -o native --cc "$CC_BIN" > /dev/null 2> cerr.txt ); then
            ( cd "$w" && ./native < "$stdin_file" > nout.txt 2> nerr.txt )
            sed 's/\[cob_interp\]/[cob]/' "$w/err.txt" > "$w/err_norm.txt"
            if cmp -s "$w/nout.txt" "$exp" && cmp -s "$w/err_norm.txt" "$w/nerr.txt"; then ok "compiled $name"
            else bad "compiled $name" "diff: $(diff "$w/nout.txt" "$exp" | head -4 | tr '\n' '|') $(diff "$w/err_norm.txt" "$w/nerr.txt" | head -4 | tr '\n' '|')"; fi
        else bad "compiled $name" "popcorn_comp failed: $(tail -2 "$w/cerr.txt" | tr '\n' ' ')"; fi
    fi
done

# ---- REPL tests (interpreter only)
for inp in "$TESTS"/repl_*.stdin; do
    [ -f "$inp" ] || continue
    name=$(basename "$inp" .stdin)
    "$INTERP" --repl < "$inp" > "$T/$name.out" 2> "$T/$name.err"
    if cmp -s "$T/$name.out" "$TESTS/$name.expected"; then ok "repl     $name"
    else bad "repl     $name" "diff: $(diff "$T/$name.out" "$TESTS/$name.expected" | head -4 | tr '\n' '|')"; fi
done

# ---- scripts
for s in errors cache static repl_modes porch_cases; do
    [ -f "$TESTS/$s.sh" ] || continue
    if sh "$TESTS/$s.sh" "$INTERP" "$COMP" "$CC_BIN" > "$T/$s.log" 2>&1; then ok "script   $s"
    else bad "script   $s" "$(tail -3 "$T/$s.log" | tr '\n' '|')"; fi
done

echo ""
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
