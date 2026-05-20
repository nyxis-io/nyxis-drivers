# CI setup (private repositories)

`nyxis-io/nyxis` and `nyxis-io/nyxis-drivers` are **private**. Driver workflows clone the core repo to build `bench/fixtures/`.

Add a repository secret on **nyxis-drivers** (and **nyxis** for conformance):

| Secret | Value |
|--------|--------|
| `NYXIS_CI_AUTOMATION_TOKEN` | GitHub PAT (classic) or fine-grained token with **read** access to `nyxis-io/nyxis` and `nyxis-io/nyxis-drivers` |

Alternatively, in the org **Settings → Actions → General**, set workflow access to allow jobs from these repositories to access each other.

Without this, `setup-core` and conformance driver checkouts fail with `Not Found`.

## Registry publishing (trusted publishers)

PyPI and NuGet use **OIDC trusted publishing** from GitHub Actions — no long-lived `PYPI_API_TOKEN` or `NUGET_API_KEY` secrets.

| Registry | Workflow | Configure on |
|----------|----------|--------------|
| PyPI | `publish-pypi.yml` | [pypi.org](https://pypi.org) → project **nyxis** → Publishing → Add trusted publisher → Owner `nyxis-io`, repo `nyxis-drivers`, workflow `publish-pypi.yml` |
| NuGet | `publish-nuget.yml` | [nuget.org](https://www.nuget.org) → package **nyxis** → Trusted Publishing → same repo + workflow `publish-nuget.yml` |

**NuGet only:** set repository variable `NUGET_USERNAME` to your nuget.org account name (used by `NuGet/login@v1`).

Other registries (npm, RubyGems, Packagist) still use repository secrets until migrated to their trusted-publisher flows.
