# Nyxis drivers

MIT-licensed language SDKs for reading and writing [Nyxis](https://github.com/nyxis-io/nyxis) `.nxb` binary payloads and running benchmarks against shared fixtures.

The **spec, Rust compiler, conformance vectors, demos, and bench UI** live in [`nyxis-io/nyxis`](https://github.com/nyxis-io/nyxis) (BSL 1.1). This repo is the embeddable surface for applications and edge runtimes.

## Layout

| Directory | Language | Notes |
| --- | --- | --- |
| [`c/`](./c/) | C99 | Native reader; used by Python/Ruby/PHP extensions |
| [`go/`](./go/) | Go 1.21+ | `github.com/nyxis-io/nyxis-drivers/go` |
| [`py/`](./py/) | Python 3 | Pure reader + optional C extension |
| [`js/`](./js/) | JavaScript | Node reader; WASM build consumes core artifacts |
| [`ruby/`](./ruby/) | Ruby | Pure + C extension |
| [`php/`](./php/) | PHP | Pure + C extension |
| [`kotlin/`](./kotlin/) | Kotlin/JVM | Gradle project |
| [`csharp/`](./csharp/) | C# | .NET |
| [`swift/`](./swift/) | Swift | macOS/iOS reader |

Each language folder has its own README with install snippets and API examples.

## Quick start

Clone **nyxis** as a sibling (or set `CORE` to your checkout):

```bash
git clone https://github.com/nyxis-io/nyxis.git ../nyxis
git clone https://github.com/nyxis-io/nyxis-drivers.git
cd nyxis-drivers
```

Generate fixtures from the core compiler, then run all driver tests:

```bash
make fixtures    # writes to ../nyxis/bench/fixtures by default
make test        # all languages
make test-go     # single language
```

In the [nyxis monorepo workspace](https://github.com/nyxis-io/nyxis), use `make -C nyxis-drivers test` from the parent tree.

## Conformance

Cross-repo conformance is driven from **nyxis**: vectors are generated in `nyxis/conformance/`, runners import drivers from this repo. See [CONFORMANCE.md](https://github.com/nyxis-io/nyxis/blob/main/CONFORMANCE.md) in the core repo (or the copy in a monorepo checkout).

## CI

Workflows check out private **`nyxis-io/nyxis`** to build fixtures. Both repos need the **`NYXIS_CI_AUTOMATION_TOKEN`** secret (classic PAT with `repo`, or fine-grained **Contents: Read** on both repositories). Details: [`.github/workflows/README.md`](./.github/workflows/README.md).

## Magic bytes (v1)

| Layer | Tag | Value |
| --- | --- | --- |
| File | `NYXB` | `0x4E595842` |
| Object | `NYXO` | `0x4E59584F` |
| List | `NYXL` | `0x4E59584C` |

Regenerate fixtures after upgrading from legacy `NXSB`/`NXSO`/`NXSL` layouts.

## License

[MIT](./LICENSE) — Copyright (c) 2026-Present Micael Malta.
