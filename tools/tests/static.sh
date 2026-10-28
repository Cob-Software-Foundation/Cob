#!/bin/sh
# static.sh -- the COB_STDLIB_STATIC build (what the browser/WASM build uses):
# stdlib modules compiled in and bound by name, wrappers embedded, so `shuck`
# works with NO standard library directory at all.
cd "$(dirname "$0")/../.." || exit 1
make cob_interp_static > /dev/null 2>&1 || { echo "make cob_interp_static failed"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

cd "$T" || exit 1
# a pure-module program, no files on disk, stdlib dir pointed at nothing
cat > pure.cob <<'XEOF'
shuck porch
shuck cornmath
shuck kernels
set name = porch_ask("name? ")
pop(kernel_upper(name) + " " + ear_max(3, 9) + " " + kernel_len(name))
XEOF
out=$(echo corn | COB_STDLIB=/nonexistent "$OLDPWD/bin/cob_interp_static" pure.cob --no-cache 2>&1)
[ "$out" = "name? CORN 9 4" ] || { echo "static pure run wrong: $out"; exit 1; }
echo "static build works"
