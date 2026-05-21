#!/usr/bin/env bash
# Copy MIT JS SDK modules into extension/lib/ before packaging.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT_LIB="$(cd "$(dirname "$0")" && pwd)/lib"
mkdir -p "$EXT_LIB"
cp "$ROOT/js/nxs.js" "$ROOT/js/nxs_decode.js" "$EXT_LIB/"
echo "Synced to $EXT_LIB"
