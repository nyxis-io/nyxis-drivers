---
room: extension
subdomain: devtools
source_paths: devtools-extension/
see_also: ["devtools/panel.md", "js/prefetch_compile.md"]
hot_paths: devtools.js, panel.js, lib/nxs_decode.js
architectural_health: normal
security_tier: normal
---

# devtools-extension/ — Nyxis Inspector

Subdomain: devtools-extension/
Source paths: devtools-extension/

## TASK → LOAD

| Task | Load |
|------|------|
| Register DevTools panel and Network hooks | extension.md |
| Render decoded `.nxs` in sidebar | extension.md |
| Refresh vendored reader after `js/` changes | extension.md |
| Panel UI / manifest / service worker | panel.md |

---

# context.js

DOES: Content-script bridge exposing minimal page context hooks for DevTools messaging (when needed for inspected tab coordination).
SYMBOLS:
- (IIFE message bridge)
DEPENDS: chrome.runtime
USE WHEN: Cross-origin inspected page coordination issues.

---

# devtools-register.js

DOES: Registers the Nyxis DevTools panel HTML and wires `devtools.panels.create` with icon and label.
SYMBOLS:
- (devtools API registration on load)
DEPENDS: devtools.html
USE WHEN: Panel missing from toolbar — verify this script loads from manifest devtools_page.

---

# devtools.html

DOES: Minimal HTML shell loading `devtools-register.js` as the DevTools bootstrap page.
SYMBOLS:
- (static HTML)
USE WHEN: DevTools extension entry point referenced by manifest.

---

# devtools.js

DOES: Core DevTools script: listens to `devtools.network` for `.nxb` responses (NYXB magic), requests body bytes, invokes `decodeToNxs`, and posts results to the panel UI.
SYMBOLS:
- onRequestFinished listener
- decodeAndSend(url, requestId) async
DEPENDS: lib/nxs_decode.js
PATTERNS: network tap, async body fetch
USE WHEN: Panel stays empty — ensure DevTools opened before request and URL ends with `.nxb`.

---

# lib/nxs.js

DOES: Vendored copy of `js/nxs.js` reader used inside the extension sandbox; updated via `sync-lib.sh`.
SYMBOLS:
- (same as js/reader.md NxsReader exports)
DEPENDS: sync-lib.sh
USE WHEN: Do not edit by hand — sync from `../js/nxs.js`.

---

# lib/nxs_decode.js

DOES: Vendored copy of `js/nxs_decode.js` for binary-to-text display in the Inspector panel.
SYMBOLS:
- decodeToNxs(input, options) string
DEPENDS: sync-lib.sh
USE WHEN: Decode formatting bugs — fix upstream in `js/nxs_decode.js` then sync.

---

# sync-lib.sh

DOES: Copies `js/nxs.js` and `js/nxs_decode.js` into `devtools-extension/lib/` after SDK changes.
SYMBOLS:
- (bash cp script)
USE WHEN: After any edit to core JS reader or decode — run before reloading unpacked extension.
