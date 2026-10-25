/* THE PROTOTYPE-SET DOCUMENT: Object.setPrototypeOf over an unknown record (ECMAScript §20.1.2.23, reaching the
   record's [[SetPrototypeOf]]), the shape Babel's and TypeScript's class helpers give a server-supplied object.
   The record is JSON.parse of location.hash. An unknown denotes an ordinary value, so after the set its
   prototype is the one written: getPrototypeOf answers it and instanceof holds.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_PROTOSET_H
#define ENGINE_HOST_FIXTURE_PROTOSET_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_PROTOSET[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  function Base() {}"
    "  var rec = JSON.parse(location.hash.slice(1));"
    "  Object.setPrototypeOf(rec, Base.prototype);"
    "  fetch('/api/pro-set?same=' + (Object.getPrototypeOf(rec) === Base.prototype) +"
    "        '&inst=' + (rec instanceof Base));"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
