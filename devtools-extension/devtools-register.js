/**
 * Register the Nyxis panel synchronously (before ES module load).
 * If devtools.js fails to import, the tab still appears with a static panel.
 */
(function () {
  const api = globalThis.chrome;
  if (!api?.devtools?.panels) return;
  api.devtools.panels.create(
    "Nyxis",
    "icons/panel-icon.png",
    "panel.html",
    () => {},
  );
})();
