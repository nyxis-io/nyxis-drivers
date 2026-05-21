/**
 * Nyxis Inspector — DevTools page: network sniff + decode, relay to panel via service worker.
 */
import { decodeToNxs, isNxbBuffer } from "./lib/nxs_decode.js";
import {
  isContextInvalidated,
  CONTEXT_INVALIDATED_USER_MSG,
} from "./context.js";

const api = globalThis.chrome;
const NETWORK = api.devtools.network;

const HEARTBEAT_MS = 20_000;
const RELAY_RETRY_MS = 400;
const GET_CONTENT_TIMEOUT_MS = 45_000;

/** @type {chrome.runtime.Port | null} */
let relayPort = null;
let heartbeatTimer = null;
let relayRetryTimer = null;
/** Bumped on navigation so in-flight getContent/decode results are ignored. */
let navigationGeneration = 0;
/** Set when the extension reloads while this DevTools window is still open. */
let extensionDead = false;

function stopHeartbeat() {
  if (heartbeatTimer) {
    clearInterval(heartbeatTimer);
    heartbeatTimer = null;
  }
}

function markExtensionDead(err) {
  if (extensionDead) return;
  extensionDead = true;
  stopHeartbeat();
  if (relayRetryTimer) {
    clearTimeout(relayRetryTimer);
    relayRetryTimer = null;
  }
  relayPort = null;
  console.warn("[Nyxis Inspector]", CONTEXT_INVALIDATED_USER_MSG, err);
}

function startHeartbeat() {
  stopHeartbeat();
  heartbeatTimer = setInterval(() => {
    if (extensionDead) return;
    if (!relayPort) {
      connectRelay();
      return;
    }
    try {
      relayPort.postMessage({ type: "PING" });
    } catch (err) {
      relayPort = null;
      if (isContextInvalidated(err)) {
        markExtensionDead(err);
        return;
      }
      connectRelay();
    }
  }, HEARTBEAT_MS);
}

function scheduleRelayReconnect() {
  if (extensionDead || relayRetryTimer) return;
  relayRetryTimer = setTimeout(() => {
    relayRetryTimer = null;
    connectRelay();
  }, RELAY_RETRY_MS);
}

function connectRelay() {
  if (extensionDead) return;
  if (relayPort) {
    try {
      relayPort.disconnect();
    } catch {
      /* already dead */
    }
    relayPort = null;
  }
  stopHeartbeat();
  try {
    relayPort = api.runtime.connect({ name: "nyxis-devtools" });
    relayPort.onDisconnect.addListener(() => {
      relayPort = null;
      stopHeartbeat();
      scheduleRelayReconnect();
    });
    startHeartbeat();
    broadcast({
      type: "PANEL_STATUS",
      connected: true,
      message: "Network listener active — load a .nxb URL (DevTools must be open before the request).",
    });
  } catch (err) {
    if (isContextInvalidated(err)) {
      markExtensionDead(err);
      return;
    }
    console.error("[Nyxis Inspector] relay connect failed:", err);
    relayPort = null;
    scheduleRelayReconnect();
  }
}

connectRelay();

function broadcast(msg) {
  if (extensionDead) return;
  if (!relayPort) connectRelay();
  if (!relayPort) return;
  try {
    relayPort.postMessage(msg);
  } catch (err) {
    if (isContextInvalidated(err)) {
      markExtensionDead(err);
      return;
    }
    console.error("[Nyxis Inspector] broadcast failed:", err);
    relayPort = null;
    scheduleRelayReconnect();
    if (msg.type === "UPDATE_VIEWER" || msg.type === "DECODE_PENDING") {
      broadcast({
        type: "DECODE_ERROR",
        message: err?.message ?? "Could not send decode to panel (message too large?)",
        meta: msg.meta ?? {},
      });
    }
  }
}

function magicHex(bytes) {
  if (!bytes || bytes.byteLength < 4) return "n/a";
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(0, true);
  return `0x${v.toString(16).toUpperCase().padStart(8, "0")}`;
}

function clearViewer(reason) {
  broadcast({
    type: "CLEAR_VIEWER",
    meta: { reason },
  });
}

/**
 * DevTools returns response bodies as a string; encoding is often "base64" for binary.
 * @param {string} content
 * @param {string} encoding
 * @returns {Uint8Array}
 */
function responseToUint8Array(content, encoding) {
  if (!content) return new Uint8Array(0);
  if (encoding === "base64") {
    const bin = atob(content);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }
  const out = new Uint8Array(content.length);
  for (let i = 0; i < content.length; i++) out[i] = content.charCodeAt(i) & 0xff;
  return out;
}

function shouldInspectRequest(request) {
  const url = request.request?.url ?? "";
  if (/\.nxb(\?|#|$)/i.test(url)) return true;
  const mime = (request.response?.content?.mimeType ?? "").toLowerCase();
  if (mime.includes("octet-stream") && /\.nxb/i.test(url)) return true;
  if (/\/bench\/fixtures\//i.test(url) && request.request?.method === "GET") return true;
  return false;
}

/** Re-fetch when DevTools getContent returns empty (cached responses). */
async function fetchNxbUrl(url) {
  const res = await fetch(url, {
    cache: "no-store",
    credentials: "omit",
    redirect: "follow",
  });
  if (!res.ok) {
    throw new Error(`HTTP ${res.status} ${res.statusText}`);
  }
  return new Uint8Array(await res.arrayBuffer());
}

function decodeAndBroadcast(bytes, url, request, captureGeneration) {
  if (captureGeneration !== navigationGeneration) return;

  if (!isNxbBuffer(bytes)) {
    broadcast({
      type: "DECODE_SKIPPED",
      meta: {
        url,
        message: `Not NYXB (${bytes.byteLength} bytes, magic ${magicHex(bytes)}).`,
      },
    });
    return;
  }

  setTimeout(() => {
    if (captureGeneration !== navigationGeneration) return;
    try {
      const text = decodeToNxs(bytes);
      const size = bytes.byteLength;
      const recordMatch = text.match(/^# Nyxis decode.*(\d+) record/s);
      const recordCount = recordMatch ? Number(recordMatch[1]) : null;

      broadcast({
        type: "UPDATE_VIEWER",
        data: text,
        meta: {
          url,
          size,
          recordCount,
          method: request.request.method,
          status: request.response.status,
        },
      });
    } catch (err) {
      broadcast({
        type: "DECODE_ERROR",
        message: err?.message ?? String(err),
        meta: { url },
      });
    }
  }, 0);
}

function loadNxbBody(request, url, captureGeneration) {
  let finished = false;
  const timeoutId = setTimeout(() => {
    if (finished) return;
    finished = true;
    broadcast({
      type: "DECODE_ERROR",
      message: "Timed out loading .nxb body (Network cache or file too large).",
      meta: { url },
    });
  }, GET_CONTENT_TIMEOUT_MS);

  const finish = (fn) => {
    if (finished) return;
    finished = true;
    clearTimeout(timeoutId);
    if (captureGeneration !== navigationGeneration) return;
    fn();
  };

  const onBytes = (bytes) => {
    finish(() => decodeAndBroadcast(bytes, url, request, captureGeneration));
  };

  const onLoadError = (err, triedFetch) => {
    finish(() => {
      broadcast({
        type: "DECODE_SKIPPED",
        meta: {
          url,
          message: triedFetch
            ? `Could not load .nxb: ${err?.message ?? err}`
            : `Response body unavailable: ${err?.message ?? err}`,
        },
      });
    });
  };

  request.getContent((content, encoding) => {
    if (finished) return;
    try {
      const bytes = responseToUint8Array(content, encoding);
      if (bytes.byteLength > 0) {
        onBytes(bytes);
        return;
      }

      broadcast({
        type: "DECODE_PENDING",
        meta: {
          url,
          message: "Network cache had no body — re-fetching…",
        },
      });

      fetchNxbUrl(url)
        .then(onBytes)
        .catch((err) => onLoadError(err, true));
    } catch (err) {
      onLoadError(err, false);
    }
  });
}

function inspectRequest(request) {
  if (!shouldInspectRequest(request)) return;

  const url = request.request?.url ?? "";
  const captureGeneration = navigationGeneration;

  broadcast({
    type: "DECODE_PENDING",
    meta: { url },
  });

  loadNxbBody(request, url, captureGeneration);
}

NETWORK.onRequestFinished.addListener(inspectRequest);

NETWORK.onNavigated.addListener(() => {
  navigationGeneration += 1;
  clearViewer("navigated");
});
