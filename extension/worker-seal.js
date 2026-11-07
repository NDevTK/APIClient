// worker-seal.js — this service worker holds no network: the zone's sends leave from the offscreen document
// (lib/wire.js), and the worker only observes them (wire-events.js). Loaded right after check.js and before every
// other script of the worker, it replaces `fetch` and every other network constructor the worker's global has
// with a non-writable, non-configurable function that CHECK-fails, as lib/safe-fetch.js does in the offscreen
// document. A Chrome 155 service worker has `fetch`, `WebSocket`, `EventSource` and `WebTransport`, and neither
// `XMLHttpRequest` nor `navigator.sendBeacon`; the chrome.* calls this worker makes (declarativeNetRequest,
// cookies, tabs, scripting, webNavigation, offscreen, runtime.getContexts) work under the seal (measured).
// Named residual. Not covered: a request no sealed name makes, such as CacheStorage's `add` (measured: it sent one
// from a sealed worker). Next diff: seal CacheStorage's fetching methods here and in lib/safe-fetch.js. How its
// absence shows: wire-events.js's CHECK firing on a no-tab request of this extension with no document.
(function () {
  function seal(name, value) {
    Object.defineProperty(globalThis, name, { value: value, writable: false, configurable: false });
  }
  seal("fetch", function fetch(input) {
    CHECK_FAIL("the service worker called fetch(" + JSON.stringify(String(input)) + ") — it holds no network; " +
               "the zone's sends leave from the offscreen document (lib/wire.js)");
  });
  ["XMLHttpRequest", "WebSocket", "EventSource", "WebTransport"].forEach(function (name) {
    if (name in globalThis) seal(name, function () {
      CHECK_FAIL("the service worker constructed " + name + " — it holds no network");
    });
  });
  if (typeof globalThis.navigator === "object" && typeof globalThis.navigator.sendBeacon === "function")
    Object.defineProperty(globalThis.navigator, "sendBeacon", { writable: false, configurable: false,
      value: function () { CHECK_FAIL("the service worker called sendBeacon — it holds no network"); } });
})();
