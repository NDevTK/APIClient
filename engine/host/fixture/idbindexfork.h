/* THE IDB-INDEX-FORK DOCUMENT: Indexed Database §4.5 index(name) over an unknown name (location.hash). The store
   's' holds the indexes 'byA' and 'byB', and the page asks its handle for the index the unknown names: one world
   per index, where the index handle names it, and the world where it names none, where the member throws a
   NotFoundError. The witness fetch carries the index's name or the error's.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBINDEXFORK_H
#define ENGINE_HOST_FIXTURE_IDBINDEXFORK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBINDEXFORK[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  var r = indexedDB.open('ix', 1);"
    "  r.onupgradeneeded = function () {"
    "    var s = r.result.createObjectStore('s');"
    "    s.createIndex('byA', 'a');"
    "    s.createIndex('byB', 'b');"
    "  };"
    "  r.onsuccess = function () {"
    "    var st = r.result.transaction('s').objectStore('s'), got;"
    "    try { got = st.index(location.hash.slice(1)).name; } catch (e) { got = e.name; }"
    "    fetch('/api/idbx-index?got=' + got);"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
