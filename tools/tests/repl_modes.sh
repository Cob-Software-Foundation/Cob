#!/bin/sh
# repl_modes.sh <cob_interp> -- `-i` runs a file then continues in the REPL with
# its variables and recipes; a quiet (non-terminal) REPL prints no prompts.
INTERP=$1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
cat > lib_for_repl.cob <<'XEOF'
set base = 40
recipe plus_two(n):
    serve n + 2
pop("file ran")
XEOF
out=$(printf 'plus_two(base)\nset base = 1\nbase\n' | "$INTERP" lib_for_repl.cob -i)
expected="file ran
42
1"
[ "$out" = "$expected" ] || { echo "-i session wrong: $out"; exit 1; }
# no prompt noise when stdin is not a terminal
out=$(printf '2 * 21\n' | "$INTERP")
[ "$out" = "42" ] || { echo "quiet repl wrong: $out"; exit 1; }
# a bad program given with -i must not start the REPL
printf 'pop(nothere(1))\n' > bad.cob
if printf '1\n' | "$INTERP" bad.cob -i > /dev/null 2>&1; then echo "bad program should fail"; exit 1; fi
echo "repl modes ok"
