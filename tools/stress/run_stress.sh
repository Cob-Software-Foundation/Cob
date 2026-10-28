#!/bin/sh
# =============================================================================
# tools/stress/run_stress.sh -- Cob Toolchain stress test driver
# =============================================================================
# Builds cob_interp and the standard library (pantry = SQLite, and barn =
# windows when its raylib dependencies are available) and runs it against a
# set of deliberately heavier .cob scripts than any of the unit/smoke tests
# use, to catch things that only show up under sustained load: interpreter
# loop overhead, recipe calls and recursion, the library modules' result
# buffers, SQLite under many sequential statements, and repeated window
# open/close cycles (resource leaks in barn's raylib backend would show up
# here as the loop getting slower or eventually crashing).
#
# Runs under Xvfb (a virtual X11 display) so the barn stress test can
# actually open real windows without a physical display. If barn cannot be
# built (no X11/OpenGL headers) the window stress is SKIPPED, not failed.
#
# Usage:
#   sh tools/stress/run_stress.sh              # default: build + run once
#   sh tools/stress/run_stress.sh --iterations N   # repeat the whole suite N times
#
# Exit code is non-zero if any stress script fails or produces unexpected
# output, so this is safe to wire into CI (see .github/workflows/ci.yml) or
# run standalone via the Dockerfile in this repo's root.
# =============================================================================
set -e

ITERATIONS=1
if [ "$1" = "--iterations" ] && [ -n "$2" ]; then
    ITERATIONS="$2"
fi

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

echo "=== Cob stress test: building cob_interp + the standard library ==="
make cob_interp stdlib

BIN="$REPO_ROOT/bin/cob_interp"
if [ ! -x "$BIN" ]; then
    echo "FAIL: $BIN was not built" >&2
    exit 1
fi

HAVE_BARN=0
echo "=== Building the barn (window) module ==="
if make stdlib-barn; then
    HAVE_BARN=1
else
    echo "NOTE: barn could not be built here (X11/OpenGL dev headers missing?) -- window stress will be skipped"
fi

# barn needs a display. Start Xvfb if $DISPLAY isn't already usable
# (e.g. running outside Docker, where a real display might exist).
STARTED_XVFB=0
if ! xdpyinfo >/dev/null 2>&1; then
    echo "=== Starting Xvfb on :99 (no usable \$DISPLAY found) ==="
    Xvfb :99 -screen 0 800x600x24 >/tmp/xvfb_stress.log 2>&1 &
    XVFB_PID=$!
    STARTED_XVFB=1
    export DISPLAY=:99
    sleep 1
fi

cleanup() {
    if [ "$STARTED_XVFB" = "1" ]; then
        kill "$XVFB_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

FAILED=0

run_one() {
    name="$1"
    script="$2"
    expect_pattern="$3"
    echo ""
    echo "=== $name ==="
    start=$(date +%s.%N)
    output=$("$BIN" "$script" 2>&1 | grep -v "^INFO:") || {
        echo "FAIL: $name exited non-zero"
        echo "$output"
        FAILED=1
        return
    }
    end=$(date +%s.%N)
    elapsed=$(awk -v s="$start" -v e="$end" 'BEGIN { printf "%.3f", e - s }')
    echo "$output"
    echo "--- elapsed: ${elapsed}s ---"
    if ! echo "$output" | grep -q "$expect_pattern"; then
        echo "FAIL: $name -- expected output matching '$expect_pattern', got:"
        echo "$output"
        FAILED=1
    fi
}

i=1
while [ "$i" -le "$ITERATIONS" ]; do
    echo ""
    echo "############################################################"
    echo "# Stress pass $i of $ITERATIONS"
    echo "############################################################"

    run_one "arithmetic/loop stress (200k iterations)" \
        "tools/stress/stress_arithmetic.cob" \
        "total=19999900000 iterations=200000"

    rm -f /tmp/stress.db
    run_one "pantry (SQLite) stress (2000 sequential inserts)" \
        "tools/stress/stress_sqlite.cob" \
        "row count=2000"

    run_one "recipe stress (100k calls, fib(22), 400-deep recursion)" \
        "tools/stress/stress_recipes.cob" \
        "recipe stress: fib(22)=17711 depth=400 calls=100000"

    run_one "standard library stress (20k kernels/cornmath calls)" \
        "tools/stress/stress_stdlib.cob" \
        "stdlib stress: total=140000 upper=CORN"

    if [ "$HAVE_BARN" = "1" ]; then
        run_one "barn window stress (20 open/label/wait/close cycles)" \
            "tools/stress/stress_window.cob" \
            "opened/closed 20 windows"
    else
        echo ""
        echo "=== barn window stress: SKIPPED (module not built) ==="
    fi

    i=$((i + 1))
done

echo ""
if [ "$FAILED" = "1" ]; then
    echo "=== STRESS TEST: FAILED ==="
    exit 1
fi
echo "=== STRESS TEST: ALL PASSED ($ITERATIONS pass(es)) ==="
