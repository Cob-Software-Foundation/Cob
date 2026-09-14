#!/usr/bin/env bash
set -euo pipefail

# Check for the custom smartpass argument
if [ "${1:-}" = "--smartpass" ]; then
    echo "--- [SmartPass System Alert] ---"
    echo "Initiating 3-second countdown to exit the terminal..."
    sleep 1
    echo "Timer running: 2 seconds remaining..."
    sleep 1
    echo "Timer running: 1 second remaining..."
    sleep 1
    echo "[ERROR] OVERTIME DETECTED! 3 minutes is up!"
    echo "[ERROR] Configuration frozen. Turn your Chromebook around and go get a physical yellow paper pass."
    exit 1
fi

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

# 3. Case-Insensitive Vendor Directory Verification
VENDOR_DIR="vendor"
if [ ! -d "$VENDOR_DIR" ]; then
    echo "[ERROR] The vendor directory '$VENDOR_DIR' does not exist in the root."
    exit 1
fi

# Convert all existing folder names inside vendor/ to lowercase for robust matching
EXISTING_FOLDERS=$(find "$VENDOR_DIR" -maxdepth 1 -mindepth 1 -type d -exec basename {} \; | tr '[:upper:]' '[:lower:]')

MISSING_DEPENDENCY=0
for REQUIRED in sqlite tcl tk miniz; do
    if ! echo "$EXISTING_FOLDERS" | grep -qx "$REQUIRED"; then
        if [ "$REQUIRED" = "sqlite" ]; then
            DISPLAY_NAME="SQLite"
        else
            DISPLAY_NAME="${REQUIRED^^}"
        fi
        echo "[ERROR] Missing required vendor dependency: $DISPLAY_NAME (checked case-insensitively in $VENDOR_DIR)"
        MISSING_DEPENDENCY=1
    fi
done

if [ "$MISSING_DEPENDENCY" -eq 1 ]; then
    echo "[ERROR] Configuration failed due to missing vendor dependencies."
    exit 1
else
    echo "[OK] All required vendor dependencies (SQLite, TCL, TK, MINIZ) are present."
fi

echo "-----------------------------------"
echo "=== Configuration Complete! Running Make... ==="
echo "-----------------------------------"
echo ""

# 4. Execute Make
exec make
