/* THE IDB-NAME-ORDER DOCUMENT: Indexed Database §2.8.2's request order over an unknown name. The page opens
   'app' at version 2 with one store and keeps the connection, queues deleteDatabase('app') (blocked by that
   connection), then open(unknown) with no version, and closes the connection on a later task. The arm where the
   unknown is 'app' is a request in 'app''s queue, so it runs after the delete: no database, upgradeneeded, version
   1, no store. Opening the existing database at version 2 with its store would be running ahead of the delete.
   The witness carries `arm=<upgraded>-<version>-<stores>-<named app>`.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBNAMEORDER_H
#define ENGINE_HOST_FIXTURE_IDBNAMEORDER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBNAMEORDER[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  var r1 = indexedDB.open('app', 2);"
    "  r1.onupgradeneeded = function () { r1.result.createObjectStore('items'); };"
    "  r1.onsuccess = function () {"
    "    var first = r1.result;"
    "    indexedDB.deleteDatabase('app');"
    "    var up = 0, r2 = indexedDB.open(location.hash.slice(1));"
    "    r2.onupgradeneeded = function () { up = 1; };"
    "    r2.onsuccess = function () {"
    "      var db = r2.result;"
    "      fetch('/api/idbo-open?arm=' + up + '-' + db.version + '-' + db.objectStoreNames.length + '-' +"
    "            (db.name === 'app' ? 1 : 0));"
    "      db.close();"
    "    };"
    "    setTimeout(function () { first.close(); }, 0);"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
