/* THE IDB-NAME-FORK DOCUMENT: a database opened by a stated name, then a second open whose name is an unknown
   (location.hash). Indexed Database §5.1 step 4 asks which database that name denotes: the arm where it is the
   stated name opens the existing database at its version, so no upgradeneeded fires and its store is seen; the
   arm where it is a fresh name creates one, upgradeneeded fires and there is no store. The witness fetch
   carries which arm ran as `arm=<upgraded>-<stores>`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBNAMEFORK_H
#define ENGINE_HOST_FIXTURE_IDBNAMEFORK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBNAMEFORK[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  var r1 = indexedDB.open('app', 1);"
    "  r1.onupgradeneeded = function () { r1.result.createObjectStore('items'); };"
    "  r1.onsuccess = function () {"
    "    r1.result.close();"
    "    var up = 0, r2 = indexedDB.open(location.hash.slice(1));"
    "    r2.onupgradeneeded = function () { up = 1; };"
    "    r2.onsuccess = function () {"
    "      var db = r2.result;"
    "      fetch('/api/idbf-open?arm=' + up + '-' + db.objectStoreNames.length);"
    "      db.close();"
    "    };"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
