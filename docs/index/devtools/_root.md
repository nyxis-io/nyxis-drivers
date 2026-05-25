---
room: _root
subdomain: devtools
source_paths: devtools-extension/
see_also: ["js/prefetch_compile.md"]
architectural_health: normal
security_tier: normal
---

# DevTools — Building Router

Subdomain: devtools-extension/
Source paths: devtools-extension/

## TASK → LOAD

| Task | Load |
|------|------|
| Install or debug the Nyxis Inspector extension | extension.md |
| Sync bundled JS SDK from `js/` | extension.md |
| Decode `.nxb` Network responses in-panel | extension.md |
| Panel UI or extension permissions | panel.md |

## Rooms

| Room | Source paths | Files |
|------|-------------|-------|
| extension.md | devtools.js, lib/, sync-lib.sh, context.js, devtools-register.js | 7 |
| panel.md | panel.js, panel.html, panel.css, background.js, manifest.json | 5 |
