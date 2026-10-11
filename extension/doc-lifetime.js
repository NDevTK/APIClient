// Isolated-world content script, run at document_start in every frame: this document's lifetime port to the
// offscreen document, which lib/web-request.js reclaims the document's request records on. Chrome closes the
// port when the document is destroyed, and also when the document enters the back/forward cache, with a
// reason the receiving end reads. A document restored from the cache opens a new port, since its old one
// stays closed.

(function () {
  "use strict";
  chrome.runtime.connect({ name: "document-lifetime" });
  addEventListener("pageshow", function (e) {
    if (e.persisted) chrome.runtime.connect({ name: "document-lifetime" });
  }, true);
})();
