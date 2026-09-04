#!/usr/bin/env bash
set -euo pipefail

echo "=== Starting Configuration ==="

# 1. Check for GCC on PATH
if ! command -v gcc &> /dev/null; then
    echo "[ERROR] GCC compiler could not be found on your PATH."
    exit 1
else
    echo "[OK] Found GCC version: $(gcc -dumpversion)"
fi

# 2. Check for Make on PATH
if ! command -v make &> /dev/null; then
    echo "[ERROR] GNU Make could not be found on your PATH."
    exit 1
else
    echo "[OK] Found Make: $(make --version | head -n 1)"
fi

echo "=== Configuration Complete! Running Make... ==="
echo ""

# 3. Execute Make
exec make
