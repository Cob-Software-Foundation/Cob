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
echo "caches behave"
