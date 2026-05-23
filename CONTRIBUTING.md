# Contributing to nyxis-drivers

MIT-licensed language SDKs for the [NXS binary format](https://github.com/nyxis-io/nyxis/blob/main/SPEC.md).

## Getting started

1. Read `SPEC.md` in the core [`nyxis`](https://github.com/nyxis-io/nyxis) repo for wire-format rules.
2. Run conformance locally from the core repo: `make -C ../nyxis conformance` (or your monorepo equivalent).
3. Use path-filtered CI as a guide: changes under `go/` only run `go.yml`, etc.

## Conventions

- **TypeManifest sigils** must match the spec (`=`, `~`, `?`, `"`, `<`, `^`, `@`, …) — see Rust `nxs::consts` in the core crate.
- **Producers** (writers) should be covered by round-trip tests against Rust-generated vectors where possible.
- Match existing API naming per language (`NewWriter`, `Reader`, column reducers).

## Pull requests

- Keep PRs focused (one language or one concern).
- Include or update tests for behavioral changes.
- For core format changes, update conformance vectors in `nyxis` first, then adapt drivers.

Core repo contributions may require the CLA in [`nyxis/CONTRIBUTING.md`](https://github.com/nyxis-io/nyxis/blob/main/CONTRIBUTING.md). This repository does not use that CLA for MIT driver code.
