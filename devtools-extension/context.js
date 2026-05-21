/**
 * Shared helpers for "Extension context invalidated" (extension reloaded while DevTools open).
 */
export function isContextInvalidated(err) {
  const msg = String(err?.message ?? err ?? "");
  return msg.includes("Extension context invalidated");
}

export const CONTEXT_INVALIDATED_USER_MSG =
  "Extension was reloaded. Close DevTools completely, then reopen it on this tab.";
