#!/bin/sh
# cache.sh <cob_interp> -- a cached (.strawberry) run of a program with
# recipes and stdlib calls behaves exactly like the first, uncached run.
INTERP=$1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
cp "$(dirname "$0")/functions.cob" fn.cob
cp "$(dirname "$0")/stdlib_all.cob" std.cob
for n in fn std; do
    "$INTERP" $n.cob > first.txt 2> first.err || true      # parses, runs, writes $n.strawberry
    [ -f $n.strawberry ] || { echo "no cache written for $n"; exit 1; }
    "$INTERP" $n.cob > second.txt 2> second.err || true    # loads the cache
    cmp -s first.txt second.txt  || { echo "$n: cached stdout differs"; diff first.txt second.txt; exit 1; }
    cmp -s first.err second.err  || { echo "$n: cached stderr differs"; diff first.err second.err; exit 1; }
done
# editing the source must run the edit, not the stale cache
printf 'pop(1)\n' > edit.cob
"$INTERP" edit.cob > e1.txt 2> /dev/null
printf 'pop(2)\n' > edit.cob
"$INTERP" edit.cob > e2.txt 2> /dev/null
[ "$(cat e1.txt)" = "1" ] && [ "$(cat e2.txt)" = "2" ] || { echo "an edited program ran its old cache: got '$(cat e2.txt)', wanted 2"; exit 1; }
"$INTERP" edit.cob > e3.txt 2> /dev/null
[ "$(cat e3.txt)" = "2" ] || { echo "the rebuilt cache is wrong"; exit 1; }
echo "caches behave"
