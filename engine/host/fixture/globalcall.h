/* THE GLOBAL-CALL DOCUMENT: an absent global taken as a function is called with `window` as its this value, as an
   analytics script calls a page's metadata hook (`r.call(g, …)`), and the record it returns, which has no example,
   is enumerated. The record is a derivation over its callee, its this value and its argument, so it has an
   identity only if `window`, HTML §7.2.3's WindowProxy and the realm's ECMAScript §19.1.1 globalThis, has a name;
   with one, the enumeration forks on the record's own-key set: /api/gc-keys reports both none and one member.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GLOBALCALL_H
#define ENGINE_HOST_FIXTURE_GLOBALCALL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GLOBALCALL[] =
    "<!doctype html><html><body>"
    "<script>"
    "var gcFn = window.__gcMeta;"
    "if (typeof gcFn === 'function') {"
    "  var gcR = gcFn.call(window, { a: 1 }), gcN = 0;"
    "  if (gcR !== null && typeof gcR === 'object') { for (var gcK in gcR) gcN++; }"
    "  fetch('/api/gc-keys?n=' + gcN);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
