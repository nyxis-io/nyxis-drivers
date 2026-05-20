# CI setup (private repositories)

`nyxis-io/nyxis` and `nyxis-io/nyxis-drivers` are **private**. Driver workflows clone the core repo to build `bench/fixtures/`.

Add a repository secret on **nyxis-drivers** (and **nyxis** for conformance):

| Secret | Value |
|--------|--------|
| `NYXIS_CI_AUTOMATION_TOKEN` | GitHub PAT (classic) or fine-grained token with **read** access to `nyxis-io/nyxis` and `nyxis-io/nyxis-drivers` |

Alternatively, in the org **Settings → Actions → General**, set workflow access to allow jobs from these repositories to access each other.

Without this, `setup-core` and conformance driver checkouts fail with `Not Found`.
