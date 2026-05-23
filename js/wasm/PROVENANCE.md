# WASM binary provenance

## `nxs_reducers.wasm`

| Field | Value |
| --- | --- |
| Source | `nxs_reducers.wat` (when present) or C reference in `nyxis/bench/wasm/` |
| Toolchain | [WABT](https://github.com/WebAssembly/wabt) `wat2wasm` ≥ 1.0.34 |
| Canonical CI OS | `ubuntu-latest` (see `.github/workflows/javascript.yml`) |
| Reproducibility | Pin WABT in CI; same `.wat` + same WABT version → identical `.wasm` bytes |

If the committed `.wasm` differs from a local macOS build, treat the **Linux CI artifact** as canonical for PRs, or rebuild with the pinned WABT version documented in [`README.md`](README.md).

## `nxs_compile_wasm_bg.wasm` (Rust compiler WASM)

Built from the `nyxis` core crate via `js/build_compile_wasm.sh` (`wasm32-unknown-unknown` + `wasm-bindgen`). Not required for the MIT reader reducers path.
