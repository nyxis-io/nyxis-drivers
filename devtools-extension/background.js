/**
 * Relays messages between the DevTools page (network decode) and the Nyxis panel.
 * MV3 service workers sleep when idle; the devtools page sends periodic pings to keep this alive.
 */
const api = globalThis.chrome ?? globalThis.browser;

/** @type {chrome.runtime.Port | null} */
let devtoolsPort = null;
/** @type {Set<chrome.runtime.Port>} */
const panelPorts = new Set();

let devtoolsDisconnectTimer = null;

function fanoutToPanels(msg) {
  for (const port of panelPorts) {
    try {
      port.postMessage(msg);
    } catch {
      panelPorts.delete(port);
    }
  }
}

function notifyPanelStatus() {
  fanoutToPanels({
    type: "PANEL_STATUS",
    connected: Boolean(devtoolsPort),
    message: devtoolsPort
      ? "Connected — open Network and load a .nxb response."
      : "Waiting for DevTools bridge…",
  });
}

function scheduleDevtoolsGoneNotice() {
  if (devtoolsDisconnectTimer) clearTimeout(devtoolsDisconnectTimer);
  devtoolsDisconnectTimer = setTimeout(() => {
    devtoolsDisconnectTimer = null;
    if (!devtoolsPort) notifyPanelStatus();
  }, 2500);
}

api.runtime.onConnect.addListener((port) => {
  if (port.name === "nyxis-devtools") {
    if (devtoolsDisconnectTimer) {
      clearTimeout(devtoolsDisconnectTimer);
      devtoolsDisconnectTimer = null;
    }
    devtoolsPort = port;
    port.onDisconnect.addListener(() => {
      devtoolsPort = null;
      scheduleDevtoolsGoneNotice();
    });
    port.onMessage.addListener((msg) => {
      if (msg?.type === "PING") {
        try {
          port.postMessage({ type: "PONG" });
        } catch {
          /* port closing */
        }
        return;
      }
      fanoutToPanels(msg);
    });
    notifyPanelStatus();
    return;
  }

  if (port.name === "nyxis-inspector-panel") {
    panelPorts.add(port);
    port.onDisconnect.addListener(() => panelPorts.delete(port));
    port.postMessage({
      type: "PANEL_STATUS",
      connected: Boolean(devtoolsPort),
      message: devtoolsPort
        ? "Connected — open Network and load a .nxb response."
        : "Waiting for DevTools bridge…",
    });
    return;
  }
});
