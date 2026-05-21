/**
 * Relays messages between the DevTools page (network decode) and the Nyxis panel.
 * MV3 panels cannot reliably message the devtools page directly; the service worker is the hub.
 */
const api = globalThis.chrome ?? globalThis.browser;

/** @type {chrome.runtime.Port | null} */
let devtoolsPort = null;
/** @type {Set<chrome.runtime.Port>} */
const panelPorts = new Set();

function fanoutToPanels(msg) {
  for (const port of panelPorts) {
    try {
      port.postMessage(msg);
    } catch {
      panelPorts.delete(port);
    }
  }
}

api.runtime.onConnect.addListener((port) => {
  if (port.name === "nyxis-devtools") {
    devtoolsPort = port;
    port.onDisconnect.addListener(() => {
      devtoolsPort = null;
      fanoutToPanels({
        type: "PANEL_STATUS",
        connected: false,
        message: "DevTools bridge closed. Close and reopen DevTools.",
      });
    });
    port.onMessage.addListener((msg) => fanoutToPanels(msg));
    fanoutToPanels({
      type: "PANEL_STATUS",
      connected: true,
      message: "Connected — open Network and load a .nxb response.",
    });
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
        : "DevTools bridge not ready. Close and reopen DevTools, then select this tab again.",
    });
    return;
  }
});
