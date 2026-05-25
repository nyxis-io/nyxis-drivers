---
room: _root
subdomain: langs
source_paths: ruby/, php/, kotlin/, csharp/, swift/
see_also: ["_root.md"]
architectural_health: normal
security_tier: normal
---

# Languages — Building Router

Subdomain: langs/
Source paths: ruby/, php/, kotlin/, csharp/, swift/

## TASK → LOAD

| Task | Load |
|------|------|
| Read or write .nxb from Ruby | ruby.md |
| Use the Ruby C extension | ruby.md |
| Read or write .nxb from PHP | php.md |
| Use the PHP C extension | php.md |
| Read or write .nxb from Kotlin/JVM | kotlin.md |
| Read or write .nxb from C# (.NET) | csharp.md |
| Read or write .nxb from Swift | swift.md |
| Benchmark any of these language implementations | (see individual room) |
| Adaptive prefetch in Ruby/PHP/Kotlin/C#/Swift | (see individual room) |

## Rooms

| Room | Source paths | Files |
|------|-------------|-------|
| ruby.md | ruby/*.rb, ruby/ext/nxs/, ruby/nyxis.gemspec | 10 |
| php.md | php/*.php, php/nxs_ext/ | 10 |
| kotlin.md | kotlin/src/main/kotlin/nxs/, kotlin/src/test/kotlin/nxs/ | 11 |
| csharp.md | csharp/*.cs, csharp/nxs.Tests/ | 12 |
| swift.md | swift/Sources/, swift/Tests/, swift/Package.swift | 14 |
