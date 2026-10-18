// Isolated-world content script, run at document_start in every frame: this document's lifetime port to the
// offscreen document, which lib/web-request.js reclaims the document's request records on. Chrome closes the
// port when the document is destroyed, and also when the document enters the back/forward cache, with a
// reason the receiving end reads. A document restored from the cache opens a new port, since its old one
// stays closed. A port reaches only the contexts that exist when it opens, so a document that opened its
// port before the offscreen document started opens another when that document announces its start (RESHIP,
// offscreen-brain.js); two ports of one document both report its end, which the receiving end takes once.

(function () {
  "use strict";
  chrome.runtime.connect({ name: "document-lifetime" });
  addEventListener("pageshow", function (e) {
    if (e.persisted) chrome.runtime.connect({ name: "document-lifetime" });
  }, true);
  var extensionOrigin = new URL(chrome.runtime.getURL("")).origin;
  chrome.runtime.onMessage.addListener(function (msg, sender) {
    if (sender.id !== chrome.runtime.id || sender.origin !== extensionOrigin || sender.tab !== undefined) return;
    if (msg && msg.type === "RESHIP") chrome.runtime.connect({ name: "document-lifetime" });
  });
})();
