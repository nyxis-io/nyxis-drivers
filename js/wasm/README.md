# nxs_reducers.wasm

## What it does

`nxs_reducers.wasm` is a WebAssembly module that accelerates bulk numeric
reducers for the NXS JavaScript driver. It implements the following exported
functions operating directly on raw `.nxb` bytes copied into WASM linear
memory:

| Export | Signature | Purpose |
|--------|-----------|---------|
| `build_field_index` | `(dataBase, size, tailStart, n, slot, outPtr) → i32` | One-pass O(n) scan building a flat array of per-record value offsets for `slot` |
| `batch_resolve_offsets` | `(dataBase, size, tailStart, rc, slot, idxPtr, count, outPtr) → void` | Resolve value offsets for a sparse set of record indices in one call |
| `batch_get_f64` | `(dataBase, size, tailStart, rc, slot, outPtr) → void` | Extract all f64 values for a field into a caller-supplied buffer |
| `sum_f64` | `(dataBase, size, tailStart, rc, slot) → f64` | Sum all f64 values for `slot`, skipping null-bitmap absent records |
| `sum_i64` | `(dataBase, size, tailStart, rc, slot) → i64` | Sum all i64 values for `slot` |
| `min_f64` | `(dataBase, size, tailStart, rc, slot) → f64` | Minimum f64 value; use `min_max_has_result()` to check for empty column |
| `max_f64` | `(dataBase, size, tailStart, rc, slot) → f64` | Maximum f64 value; use `min_max_has_result()` to check for empty column |
| `min_max_has_result` | `() → i32` | Returns 1 if the preceding `min_f64`/`max_f64` touched at least one non-null record |
| `encode_span` | `(outPtr, fieldsPtr) → i32` | Encode a 10-field span struct into a NYXO record; returns byte count written |
| `compute_fast_layout` | internal helper | Introspects record 0 bitmask for `build_field_index` fast path |
| `field_offset` | internal helper | Resolve one record's value offset for a given slot |
| `encode_span` | `(outPtr, fieldsPtr) → i32` | Encode a canonical span struct into a raw NYXO record |

The module also exports `memory` (linear memory) and `__stack_pointer` for
the WASM runtime. The host allocates one `WebAssembly.Memory` of at least
1024 pages (64 MB) and passes it to the module via the `env` import group.
The `.nxb` payload is copied to `dataBase` (byte offset 65536 by default) and
reducers operate on it in-place — no additional copies inside the module.

## Source

This module was hand-written in WebAssembly Text Format (WAT) and assembled
with [`wat2wasm`](https://github.com/WebAssembly/wabt) from the
[WABT toolkit](https://github.com/WebAssembly/wabt). The WAT source lives at:

```
js/wasm/nxs_reducers.wat   (to be added; see "How to rebuild" below)
```

The functions match the low-level NXS binary wire format described in
`SPEC.md` (preamble, tail-index, per-record LEB128 bitmask, and 8-byte
aligned value slots).

## Intended toolchain

| Tool | Version | Role |
|------|---------|------|
| [`wabt`](https://github.com/WebAssembly/wabt) | ≥ 1.0.34 | Assemble `.wat` → `.wasm` (`wat2wasm`) and inspect (`wasm-objdump`) |
| `wasm-opt` (Binaryen) | optional | Size/speed optimisation pass |

> **Why not `wasm-pack` / Emscripten?**  
> The module is a thin numeric kernel with no Rust/C source and no standard
> library dependency. WAT gives exact control over memory layout and export
> names and avoids a Rust toolchain requirement in the JS driver tree.

## How to rebuild

```bash
# Install WABT (macOS via Homebrew)
brew install wabt

# Assemble
wat2wasm js/wasm/nxs_reducers.wat -o js/wasm/nxs_reducers.wasm

# Optional: optimise (requires Binaryen)
wasm-opt -O2 js/wasm/nxs_reducers.wasm -o js/wasm/nxs_reducers.wasm

# Verify exports
wasm-objdump -x js/wasm/nxs_reducers.wasm | grep -A 40 "Export"
```

The resulting binary should be byte-for-byte reproducible on any host that
uses the same `wat2wasm` version. Pin the WABT version in CI to ensure
reproducibility.

## Size

Current binary: **4422 bytes** (MVP implementation; no Binaryen optimisation
pass applied yet).
