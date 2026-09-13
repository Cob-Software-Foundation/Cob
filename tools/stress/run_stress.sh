#!/bin/sh
# =============================================================================
# tools/stress/run_stress.sh -- Cob Toolchain stress test driver
# =============================================================================
# Builds cob_interp_full (SQLite + _cobwindow) and runs it against a set of
# deliberately heavier .cob scripts than any of the unit/smoke tests in
# .github/workflows/build.yml use, to catch things that only show up under
# sustained load: interpreter loop overhead, SQLite under many sequential
# statements, and repeated window open/close cycles (resource leaks in
# _cobwindow's raylib backend would show up here as the loop getting slower
# or eventually crashing, not just "does it open one window").
#
# Runs under Xvfb (a virtual X11 display) so the _cobwindow stress test can
# actually open real windows without a physical display -- same approach
# used in build.yml's test_extensions job and verified there.
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

echo "=== Cob stress test: building cob_interp_full ==="
make cob_interp_full

BIN="$REPO_ROOT/bin/cob_interp_full"
if [ ! -x "$BIN" ]; then
    echo "FAIL: $BIN was not built" >&2
    exit 1
fi

# _cobwindow needs a display. Start Xvfb if $DISPLAY isn't already usable
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
    run_one "SQLite stress (2000 sequential inserts)" \
        "tools/stress/stress_sqlite.cob" \
        "row count=2000"

    run_one "_cobwindow stress (20 open/label/wait/close cycles)" \
        "tools/stress/stress_window.cob" \
        "opened/closed 20 windows"

    i=$((i + 1))
done

echo ""
if [ "$FAILED" = "1" ]; then
    echo "=== STRESS TEST: FAILED ==="
    exit 1
fi
echo "=== STRESS TEST: ALL PASSED ($ITERATIONS pass(es)) ==="
