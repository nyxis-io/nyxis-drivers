# Nyxis Drivers

MIT-licensed SDKs for reading, writing, prefetching, and benchmarking [Nyxis](https://github.com/nyxis-io/nyxis) `.nxb` binary payloads.

This repository is the embeddable application surface for Nyxis. The core spec, Rust compiler, conformance vectors, browser demos, benchmark harnesses, and MCP server live in [`nyxis-io/nyxis`](https://github.com/nyxis-io/nyxis).

## Supported Drivers

| Directory | Language | Surface |
| --- | --- | --- |
| [`c/`](./c/) | C99 | Native reader/writer; shared by extension-backed dynamic languages |
| [`go/`](./go/) | Go 1.26+ | Reader, writer, reducers, adaptive prefetch |
| [`py/`](./py/) | Python 3 | Pure reader/writer plus optional C extension |
| [`js/`](./js/) | JavaScript | Node/browser reader, writer, WASM compile/decode helpers |
| [`ruby/`](./ruby/) | Ruby | Pure reader/writer plus C extension |
| [`php/`](./php/) | PHP | Pure reader/writer plus C extension |
| [`kotlin/`](./kotlin/) | Kotlin/JVM | JVM reader and reducers |
| [`csharp/`](./csharp/) | C# | .NET reader and reducers |
| [`swift/`](./swift/) | Swift | macOS/iOS reader and reducers |
| [`devtools-extension/`](./devtools-extension/) | Browser | Nyxis Inspector DevTools panel for `.nxb` network responses |

Each language directory has its own README with install commands, API examples, and language-specific test commands.

## Install

| Ecosystem | Command or source |
| --- | --- |
| C | Download source artifacts from GitHub Releases or use [`c/`](./c/) directly |
| Go | `go get github.com/nyxis-io/nyxis-drivers/go` |
| Python | `pip install nyxis` |
| JavaScript | `npm install nyxis` |
| Ruby | `gem install nyxis` |
| PHP | `composer require nyxis/nyxis` |
| Kotlin | See [`kotlin/README.md`](./kotlin/README.md) |
| C# | `dotnet add package nyxis` |
| Swift | See [`swift/README.md`](./swift/README.md) |

## Quick Start

Clone the core repo as a sibling, generate fixtures with the Rust compiler, then run driver tests:

```bash
git clone https://github.com/nyxis-io/nyxis.git ../nyxis
git clone https://github.com/nyxis-io/nyxis-drivers.git
cd nyxis-drivers

make fixtures
make test
```

Run a single driver while iterating:

```bash
make test-go
make test-js
make test-py
```

In the multi-repo workspace, run from the parent directory:

```bash
make -C nyxis fixtures
make -C nyxis-drivers test
```

## Reader Model

All drivers read the same `.nxb` files produced by the Rust compiler in `nyxis`. Readers open a byte slice, mmap region, or language-native buffer; read the preamble and embedded schema; then use the tail-index to locate records without parsing the full payload.

Common operations across drivers:

- Open a `.nxb` payload and read `record_count`.
- Fetch record `i` in O(1) through the tail-index.
- Decode individual fields by key or slot.
- Run bulk reducers such as `sum_f64`, `min_f64`, `max_f64`, and `sum_i64`.
- Write `.nxb` bytes from schema-defined slots.

## Adaptive Prefetch

Row-layout readers support optional viewport prefetch for browser, edge, and remote-file access patterns:

| Option | Behavior |
| --- | --- |
| `hint` | Advisory access pattern: `unknown`, `sequential`, `random`, `full`, or `partial` |
| `lazy` | Default for large files where the reader should fetch only touched pages |
| `adaptive` | Mid-size default that promotes based on observed access patterns |
| `eager` | Full prefetch for small files, explicit `full` hints, or sustained sequential reads |
| `prefetch_viewport(start, end)` | Coalesces page fetches for a visible record range |
| `warmup()` | Waits for in-flight eager background load where supported |
| `cache_stats()` | Reports strategy, detected pattern, cache hits/misses, and fetch counts |

PHP eager loading is synchronous because the extension does not start a background worker. See [`php/README.md`](./php/README.md).

Prefetch conformance vectors live in [`nyxis/conformance/prefetch/`](https://github.com/nyxis-io/nyxis/tree/main/conformance/prefetch). Run them from the core repo with:

```bash
make conformance-prefetch PREFETCH=1
```

## Conformance

Cross-repo conformance is driven from `nyxis`: the core repo generates vectors in `nyxis/conformance/`, then runners import drivers from this repo. See [`CONFORMANCE.md`](https://github.com/nyxis-io/nyxis/blob/main/CONFORMANCE.md) for the vector format and test matrix.

## Magic Bytes

| Layer | Tag | Value |
| --- | --- | --- |
| File | `NYXB` | `0x4E595842` |
| Object | `NYXO` | `0x4E59584F` |
| List | `NYXL` | `0x4E59584C` |

Regenerate fixtures after upgrading from legacy `NXSB`/`NXSO`/`NXSL` layouts.

## CI

Driver workflows check out `nyxis` to build fixtures and conformance vectors. Both repositories need the `NYXIS_CI_AUTOMATION_TOKEN` secret for cross-repo jobs. Details are in [`.github/workflows/README.md`](./.github/workflows/README.md).

## License

[MIT](./LICENSE) - Copyright (c) 2026-Present Micael Malta.
