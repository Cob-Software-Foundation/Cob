#!/bin/sh
# tools/docker/smoke_test.sh <image> -- does the Cob container image actually work?
#
#   docker build -t cob . && sh tools/docker/smoke_test.sh cob
#
# Used by .github/workflows/docker.yml before anything is published[cite: 2, 3]. Checks
# the version, a program with recipes and standard library modules, the REPL on
# stdin, SQLite, compiling with popcorn_comp and running the result, the other
# tools, and that the image runs as an unprivileged user[cite: 3].
set -eu
IMG=${1:?usage: smoke_test.sh <image>}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Strip any pre-release suffix (e.g., 0.0.9-rc -> 0.0.9) to match output of cob --version[cite: 1, 3]
WANT_VERSION=$(tr -d '[:space:]' < "$HERE/../../VERSION" | cut -d'-' -f1)

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$T"
U="$(id -u):$(id -g)"   # act as ourselves so /work (a bind mount) is writable

check() {   # name expected actual
    if [ "$2" = "$3" ]; then echo "ok    $1"
    else echo "FAIL  $1" >&2; echo "      expected: $2" >&2; echo "      got:      $3" >&2; exit 1; fi
}
cob()  { docker run --rm --user "$U" -v "$T:/work" "$IMG" "$@"; }

# 1. the version matches the repo's VERSION file[cite: 3]
out=$(docker run --rm "$IMG" --version)
case "$out" in
    *"$WANT_VERSION"*) echo "ok    version ($out)" ;;
    *) echo "FAIL  version: expected $WANT_VERSION in: $out" >&2; exit 1 ;;
esac

# 2. a program: recipes + recursion + standard library modules[cite: 3]
cat > hello.cob <<'XEOF'
shuck silo
recipe fact(n):
    if n <= 1:
        serve 1
    serve n * fact(n - 1)
pop(kernel_upper("corn") + " " + fact(10))
XEOF
check "run a program" "CORN 3628800" "$(cob hello.cob)"

# 3. the REPL, reading stdin[cite: 3]
check "REPL on stdin" "3" "$(printf '1 + 2\n' | docker run --rm -i "$IMG")"

# 4. SQLite (the pantry module is a shared library the image carries)[cite: 3]
cat > db.cob <<'XEOF'
shuck pantry
set h = pantry_open(":memory:")
set r = pantry_exec(h, "CREATE TABLE t (n INTEGER)")
set r = pantry_exec(h, "INSERT INTO t VALUES (41), (1)")
pop(pantry_query(h, "SELECT SUM(n) FROM t"))
XEOF
check "pantry (SQLite)" "42" "$(cob db.cob)"

# 5. compile with popcorn_comp, then run the native program inside the image[cite: 3]
cob hello.cob > /dev/null                       # writes hello.strawberry
cob popcorn_comp hello.strawberry -o hello_native > /dev/null
check "compile and run" "CORN 3628800" \
    "$(docker run --rm --user "$U" -v "$T:/work" --entrypoint /work/hello_native "$IMG")"

# 6. the other tools are on PATH[cite: 3]
cob farmer --license > /dev/null && echo "ok    farmer runs"
check "shell available" "yes" "$(cob sh -c 'echo yes')"

# 7. not root[cite: 3]
check "runs unprivileged" "10001" "$(docker run --rm --entrypoint id "$IMG" -u)"

echo "image $IMG: all checks passed"