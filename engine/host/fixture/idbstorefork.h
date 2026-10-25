/* THE IDB-STORE-FORK DOCUMENT: Indexed Database §4.10 objectStore(name) over an unknown name (location.hash). A
   transaction over the stores 'alpha' and 'beta' is asked for the store the unknown names: one world per store
   in its scope, where the handle names that store, and the world where it names none, where the member throws
   a NotFoundError. The witness fetch carries the store's name or the error's.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBSTOREFORK_H
#define ENGINE_HOST_FIXTURE_IDBSTOREFORK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBSTOREFORK[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  var r = indexedDB.open('stores', 1);"
    "  r.onupgradeneeded = function () {"
    "    r.result.createObjectStore('alpha');"
    "    r.result.createObjectStore('beta');"
    "  };"
    "  r.onsuccess = function () {"
    "    var tx = r.result.transaction(['alpha', 'beta']), got;"
    "    try { got = tx.objectStore(location.hash.slice(1)).name; } catch (e) { got = e.name; }"
    "    fetch('/api/idbs-store?got=' + got);"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
