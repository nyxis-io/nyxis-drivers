---
room: panel
subdomain: devtools
source_paths: devtools-extension/
see_also: ["devtools/extension.md", "js/prefetch_compile.md"]
hot_paths: panel.js, panel.html
architectural_health: normal
security_tier: normal
---

# devtools-extension/ — Panel UI

Subdomain: devtools-extension/
Source paths: devtools-extension/

## TASK → LOAD

| Task | Load |
|------|------|
| Style or layout the Inspector sidebar | panel.md |
| Fix panel status / empty state UX | panel.md |
| Update MV3 manifest permissions | panel.md |

---

# background.js

DOES: MV3 service worker for heartbeat, message relay, and `.nxb` body re-fetch when Network cache lacks response bytes.
SYMBOLS:
- chrome.runtime.onMessage handlers
PATTERNS: service-worker lifecycle
USE WHEN: Extension shows disconnected or fails to fetch large `.nxb` bodies.

---

# manifest.json

DOES: Extension manifest (permissions, devtools_page, background service worker, panel resources).
CONFIG: host_permissions, manifest_version
USE WHEN: Adding new origins or DevTools assets.

---

# panel.css

DOES: Monospace decode panel styles and status bar layout.
SYMBOLS:
- (CSS only)
USE WHEN: Readability tweaks for decoded `.nxs` output.

---

# panel.html

DOES: Sidebar HTML shell loading `panel.js` and `panel.css`.
SYMBOLS:
- (static markup)
USE WHEN: Adding controls to the Inspector UI.

---

# panel.js

DOES: Panel controller: renders decoded text, loading/error states, clears on `devtools.network.onNavigated`.
SYMBOLS:
- renderDecoded(text) void
- setStatus(msg) void
DEPENDS: panel.css, chrome.runtime.onMessage
USE WHEN: Panel UI bugs — trace messages from extension.md devtools.js.
