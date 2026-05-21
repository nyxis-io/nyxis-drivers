#!/usr/bin/env bash
# Build nxs_compile.wasm + glue JS from the nyxis Rust compiler (wasm32).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
JS_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT/nyxis/rust"

if ! rustup target list --installed | grep -q wasm32-unknown-unknown; then
  rustup target add wasm32-unknown-unknown
fi

cargo build --release --target wasm32-unknown-unknown --features wasm --lib -p nyxis

WASM_BIN="$ROOT/nyxis/rust/target/wasm32-unknown-unknown/release/nxs.wasm"
if [[ ! -f "$WASM_BIN" ]]; then
  echo "error: expected $WASM_BIN" >&2
  exit 1
fi

if ! command -v wasm-bindgen >/dev/null 2>&1; then
  echo "Installing wasm-bindgen-cli…" >&2
  cargo install wasm-bindgen-cli --locked
fi

wasm-bindgen "$WASM_BIN" \
  --target web \
  --out-dir "$JS_DIR" \
  --out-name nxs_compile_wasm

echo "Built $JS_DIR/nxs_compile_wasm.js and nxs_compile_wasm_bg.wasm"
