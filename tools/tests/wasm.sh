#!/bin/sh
# wasm.sh <cob_interp> -- the browser (WebAssembly) build must match the native
# interpreter. Needs Node and a built docs/play/cob_interp.wasm (`make play`);
# without either it says so and skips.
INTERP=$1
HERE=$(cd "$(dirname "$0")" && pwd)
command -v node >/dev/null 2>&1 || { echo "no node -- skipped"; exit 0; }
[ -f "$HERE/../../docs/play/cob_interp.wasm" ] || { echo "docs/play/cob_interp.wasm not built (make play) -- skipped"; exit 0; }
node "$HERE/wasm_test.js" "$INTERP"
