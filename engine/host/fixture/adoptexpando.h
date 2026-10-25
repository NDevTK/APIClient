/* THE ADOPT EXPANDO DOCUMENT: an element created in a same-origin popup's document carries an expando, is adopted
   into this document by being appended to it, and the popup is closed and every script reference to it dropped.
   The heap is then grown so collections run, and the element is read back through this document's tree. A browser
   keeps the element's wrapper, and its expando, while the element is in a live tree; this engine's identity map
   entry belongs to the wrapper's creation realm (core/dom/node.c's RealmWraps residual), so once the popup's realm
   is collected the element's next wrapper is a new object. One of test_forced.c's exploring documents. It ends
   with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ADOPTEXPANDO_H
#define ENGINE_HOST_FIXTURE_ADOPTEXPANDO_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ADOPTEXPANDO[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "(async function () {"
    " var w = open(), d = w.document, el = d.createElement('div');"
    " el.axp = 'kept';"
    " document.body.appendChild(el);"
    " w.close(); w = null; d = null; el = null;"
    " for (var k = 0; k < 30; k++) {"
    "  var junk = []; for (var i = 0; i < 20000; i++) junk.push({ i: i });"
    "  await new Promise(function (r) { setTimeout(r, 0); });"
    " }"
    " var got = document.body.lastChild.axp;"
    " fetch('/api/adoptexp?v=' + (got === 'kept' ? 'kept' : 'lost'));"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
