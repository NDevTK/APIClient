/* THE IDB-SCOPE-FORK DOCUMENT: Indexed Database §4.4 transaction(storeNames) over an unknown name
   (location.hash). Steps 3-4 resolve each name to an object store of the database or throw a NotFoundError, so
   an unknown name is one world per store ('alpha', 'beta'), where the transaction's scope is that store, and the
   world where it names none. The witness fetch carries the scope's first name or the error's.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBSCOPEFORK_H
#define ENGINE_HOST_FIXTURE_IDBSCOPEFORK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBSCOPEFORK[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  var r = indexedDB.open('scope', 1);"
    "  r.onupgradeneeded = function () {"
    "    r.result.createObjectStore('alpha');"
    "    r.result.createObjectStore('beta');"
    "  };"
    "  r.onsuccess = function () {"
    "    var got;"
    "    try { got = r.result.transaction(location.hash.slice(1)).objectStoreNames[0]; } catch (e) { got = e.name; }"
    "    fetch('/api/idbsc-tx?got=' + got);"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
