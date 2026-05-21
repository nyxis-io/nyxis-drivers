const api = globalThis.chrome;

const metaEl = document.getElementById("meta");
const viewerEl = document.getElementById("viewer");
const copyBtn = document.getElementById("copy");
const wrapBtn = document.getElementById("wrap");

const MAX_RECONNECT_ATTEMPTS = 12;
const RECONNECT_BASE_MS = 400;

let lastText = "";
let port = null;
let reconnectTimer = null;
let reconnectAttempt = 0;

function connectPanel() {
  if (port) {
    try {
      port.disconnect();
    } catch {
      /* already closed */
    }
    port = null;
  }

  try {
    port = api.runtime.connect({ name: "nyxis-inspector-panel" });
    reconnectAttempt = 0;
  } catch (err) {
    scheduleReconnect(`Could not connect: ${err?.message ?? err}`);
    return;
  }

  port.onDisconnect.addListener(() => {
    port = null;
    scheduleReconnect();
  });

  port.onMessage.addListener(onMessage);
}

function scheduleReconnect(statusText) {
  if (reconnectTimer) return;

  if (reconnectAttempt >= MAX_RECONNECT_ATTEMPTS) {
    setStatus(
      "Disconnected — reload the extension at chrome://extensions, then reopen DevTools.",
      true,
    );
    return;
  }

  if (statusText) {
    setStatus(statusText, true);
  } else {
    setStatus("Reconnecting…", false);
  }

  const delay = Math.min(RECONNECT_BASE_MS * (2 ** reconnectAttempt), 8000);
  reconnectAttempt += 1;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connectPanel();
  }, delay);
}

function setStatus(text, isError = false) {
  metaEl.textContent = text;
  metaEl.classList.toggle("error", isError);
}

function clearPanel(message) {
  lastText = "";
  viewerEl.textContent = "";
  viewerEl.classList.remove("error", "decoding");
  copyBtn.disabled = true;
  setStatus(message ?? "Waiting for an .nxb response in the Network tab…");
}

function onMessage(msg) {
  if (msg.type === "PANEL_STATUS") {
    viewerEl.classList.remove("decoding");
    setStatus(msg.message ?? "", !msg.connected);
    return;
  }

  if (msg.type === "CLEAR_VIEWER") {
    clearPanel("Page navigated — waiting for a new .nxb response…");
    return;
  }

  if (msg.type === "DECODE_PENDING") {
    const url = msg.meta?.url;
    viewerEl.classList.add("decoding");
    viewerEl.textContent = "";
    copyBtn.disabled = true;
    setStatus(url ? `Fetching & decoding ${shortenUrl(url)}…` : "Fetching & decoding…");
    return;
  }

  if (msg.type === "DECODE_SKIPPED") {
    viewerEl.classList.remove("decoding");
    return;
  }

  if (msg.type === "UPDATE_VIEWER") {
    lastText = msg.data ?? "";
    viewerEl.textContent = lastText;
    viewerEl.classList.remove("error", "decoding");
    copyBtn.disabled = !lastText;

    const m = msg.meta ?? {};
    const parts = [];
    if (m.method && m.status != null) parts.push(`${m.method} ${m.status}`);
    if (m.size != null) parts.push(`${formatBytes(m.size)}`);
    if (m.recordCount != null) parts.push(`${m.recordCount} record(s)`);
    const tail = parts.length ? ` — ${parts.join(" · ")}` : "";
    const shortUrl = shortenUrl(m.url);
    metaEl.classList.remove("error");
    metaEl.innerHTML = shortUrl
      ? `<span title="${escapeAttr(m.url)}">${escapeHtml(shortUrl)}</span>${escapeHtml(tail)}`
      : "Decoded .nxb response";
    return;
  }

  if (msg.type === "DECODE_ERROR") {
    lastText = "";
    viewerEl.textContent = `Decode error: ${msg.message}`;
    viewerEl.classList.add("error");
    viewerEl.classList.remove("decoding");
    copyBtn.disabled = true;
    metaEl.classList.add("error");
    metaEl.textContent = msg.meta?.url ? shortenUrl(msg.meta.url) : "Error";
  }
}

connectPanel();

copyBtn.addEventListener("click", async () => {
  if (!lastText) return;
  try {
    await navigator.clipboard.writeText(lastText);
    copyBtn.textContent = "Copied";
    setTimeout(() => { copyBtn.textContent = "Copy"; }, 1200);
  } catch {
    copyBtn.textContent = "Failed";
  }
});

wrapBtn.addEventListener("click", () => {
  viewerEl.classList.toggle("wrap");
});

function formatBytes(n) {
  if (n < 1024) return `${n} B`;
  if (n < 1048576) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1048576).toFixed(1)} MB`;
}

function shortenUrl(url) {
  if (!url) return "";
  try {
    const u = new URL(url);
    return u.pathname.split("/").pop() || url;
  } catch {
    return url.length > 48 ? `${url.slice(0, 45)}…` : url;
  }
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  }[c]));
}

function escapeAttr(s) {
  return escapeHtml(s).replace(/`/g, "&#96;");
}
