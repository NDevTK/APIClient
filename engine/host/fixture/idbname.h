/* THE IDB-NAME DOCUMENT: Indexed Database names taken from location.hash, which the attacker sets. The page
   opens a database whose name is an unknown (§4.3 open), creates an object store whose name is another in the
   upgrade, counts the stores on success, closes and deletes the database through the same unknown name
   (§4.3 deleteDatabase), and reports the count once the delete succeeds. One unknown value names one database.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBNAME_H
#define ENGINE_HOST_FIXTURE_IDBNAME_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBNAME[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  var n = 'db' + location.hash, r = indexedDB.open(n, 1);"
    "  r.onupgradeneeded = function () { r.result.createObjectStore('s' + location.hash); };"
    "  r.onsuccess = function () {"
    "    var db = r.result, stores = db.objectStoreNames.length;"
    "    db.close();"
    "    indexedDB.deleteDatabase(n).onsuccess = function () { fetch('/api/idbn-deleted?stores=' + stores); };"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
