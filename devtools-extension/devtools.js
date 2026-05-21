/**
 * Nyxis Inspector — DevTools page: network sniff + decode, relay to panel via service worker.
 */
import { decodeToNxs, isNxbBuffer } from "./lib/nxs_decode.js";

const api = globalThis.chrome;
const NETWORK = api.devtools.network;

const HEARTBEAT_MS = 20_000;
const RELAY_RETRY_MS = 400;

/** @type {chrome.runtime.Port | null} */
let relayPort = null;
let heartbeatTimer = null;
let relayRetryTimer = null;
/** Bumped on navigation so in-flight getContent/decode results are ignored. */
let navigationGeneration = 0;

function stopHeartbeat() {
  if (heartbeatTimer) {
    clearInterval(heartbeatTimer);
    heartbeatTimer = null;
  }
}

function startHeartbeat() {
  stopHeartbeat();
  heartbeatTimer = setInterval(() => {
    if (!relayPort) {
      connectRelay();
      return;
    }
    try {
      relayPort.postMessage({ type: "PING" });
    } catch {
      relayPort = null;
      connectRelay();
    }
  }, HEARTBEAT_MS);
}

function scheduleRelayReconnect() {
  if (relayRetryTimer) return;
  relayRetryTimer = setTimeout(() => {
    relayRetryTimer = null;
    connectRelay();
  }, RELAY_RETRY_MS);
}

function connectRelay() {
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
    console.error("[Nyxis Inspector] relay connect failed:", err);
    relayPort = null;
    scheduleRelayReconnect();
  }
}

connectRelay();

function broadcast(msg) {
  if (!relayPort) connectRelay();
  if (!relayPort) return;
  try {
    relayPort.postMessage(msg);
  } catch (err) {
    console.error("[Nyxis Inspector] broadcast failed:", err);
    relayPort = null;
    scheduleRelayReconnect();
  }
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

function inspectRequest(request) {
  if (!shouldInspectRequest(request)) return;

  const url = request.request?.url ?? "";
  const captureGeneration = navigationGeneration;

  broadcast({
    type: "DECODE_PENDING",
    meta: { url },
  });

  request.getContent((content, encoding) => {
    if (captureGeneration !== navigationGeneration) return;

    try {
      const bytes = responseToUint8Array(content, encoding);
      if (!isNxbBuffer(bytes)) {
        broadcast({ type: "DECODE_SKIPPED", meta: { url } });
        return;
      }

      // Yield so DECODE_PENDING can paint before a large synchronous decode.
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
    } catch (err) {
      broadcast({
        type: "DECODE_ERROR",
        message: err?.message ?? String(err),
        meta: { url },
      });
    }
  });
}

NETWORK.onRequestFinished.addListener(inspectRequest);

NETWORK.onNavigated.addListener(() => {
  navigationGeneration += 1;
  clearViewer("navigated");
});
