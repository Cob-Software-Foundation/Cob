#!/bin/sh
# popcorn_comp --static-stdlib: the finished program must run with no standard library directory anywhere.
# usage: static_stdlib.sh INTERP COMP CC
INTERP=$1; COMP=$2; CC_BIN=$3
LIBDIR="$(dirname "$INTERP")/lib"
[ -n "$CC_BIN" ] || { echo "skipped: no C compiler"; exit 0; }
[ -f "$LIBDIR/libcob_stdlib.a" ] || { echo "skipped: no libcob_stdlib.a (run make stdlib)"; exit 0; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cd "$T" || exit 1
# bushel (lists), kernels (text) and cornmath (numbers) all come from the archive
cat > prog.cob <<'COB'
shuck bushel
shuck kernels
shuck cornmath
set xs = [3, 1, 2]
set xs[0] = 30   # trailing comment
pop(bushel_str(xs))
pop(kernel_upper("static"))
pop(husk_abs(0 - 7))
COB
"$INTERP" prog.cob > /dev/null 2>&1 || exit 1                          # writes prog.strawberry
"$COMP" prog.strawberry -o prog_static --cc "$CC_BIN" --static-stdlib > /dev/null 2> cerr.txt || { cat cerr.txt; exit 1; }
"$COMP" prog.strawberry -o prog_dyn --cc "$CC_BIN" --dynamic-stdlib > /dev/null 2>&1 || exit 1
# 1. no shared standard library module is a dependency, and there is no rpath into lib/
if command -v readelf > /dev/null 2>&1; then
    readelf -d prog_static | grep -E "NEEDED|RPATH|RUNPATH" | grep -E "bushel|kernels|cornmath|/lib" && { echo "static program still depends on the shared library directory"; exit 1; }
    readelf -d prog_dyn | grep -q "libbushel" || { echo "control: the dynamic program should need libbushel"; exit 1; }
fi
# 2. it runs and prints the same thing as the interpreter, with no library directory reachable
mkdir empty
env -i COB_STDLIB="$T/empty" ./prog_static > out_static.txt 2> /dev/null || { echo "static program failed to run"; exit 1; }
"$INTERP" prog.cob --no-cache > out_interp.txt 2> /dev/null
cmp -s out_static.txt out_interp.txt || { echo "output differs from the interpreter"; diff out_static.txt out_interp.txt; exit 1; }
# 3. asking for it with the archive missing is an error that says what to do
mkdir fake && cp "$INTERP" fake/cob_interp
if COB_STDLIB="$T/empty" "$COMP" prog.strawberry -o prog_x --cc "$CC_BIN" --static-stdlib > /dev/null 2> e2.txt; then echo "expected an error without libcob_stdlib.a"; exit 1; fi
grep -q "libcob_stdlib.a" e2.txt || { echo "error should name libcob_stdlib.a"; cat e2.txt; exit 1; }
echo "ok"
