/**
 * Nyxis Inspector — DevTools background for the custom panel.
 * Sniffs Network responses for NYXB (.nxb) payloads and sends decoded .nxs text to the panel.
 */
import { decodeToNxs, isNxbBuffer } from "./lib/nxs_decode.js";

const api = globalThis.chrome ?? globalThis.browser;
const NETWORK = api.devtools.network;

/** @type {Set<chrome.runtime.Port>} */
const panelPorts = new Set();

api.runtime.onConnect.addListener((port) => {
  if (port.name !== "nyxis-inspector-panel") return;
  panelPorts.add(port);
  port.onDisconnect.addListener(() => panelPorts.delete(port));
});

function broadcast(msg) {
  for (const port of panelPorts) {
    try {
      port.postMessage(msg);
    } catch {
      panelPorts.delete(port);
    }
  }
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
  const mime = request.response?.content?.mimeType ?? "";
  if (mime.includes("octet-stream") && /\.nxb/i.test(url)) return true;
  return false;
}

function inspectRequest(request) {
  if (!shouldInspectRequest(request)) return;

  request.getContent((content, encoding) => {
    try {
      const bytes = responseToUint8Array(content, encoding);
      if (!isNxbBuffer(bytes)) return;

      const text = decodeToNxs(bytes);
      const url = request.request.url;
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
        meta: { url: request.request?.url },
      });
    }
  });
}

api.devtools.panels.create(
  "Nyxis",
  "",
  "panel.html",
  () => {},
);

NETWORK.onRequestFinished.addListener(inspectRequest);
