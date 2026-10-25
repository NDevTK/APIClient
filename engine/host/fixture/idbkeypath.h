/* THE IDB-KEY-PATH DOCUMENT: Indexed Database §4.4 createObjectStore and §4.5 createIndex with key paths read
   from a fetched JSON reply. §2.5's valid key path is a test of bytes the reply's fields do not hold, so each
   call runs its valid world and its SyntaxError world (an unknown string refused, or §3.2.25's sequence arm
   with no entries). With autoIncrement, §4.4 step 8 also refuses an empty string or any sequence with an
   InvalidAccessError. Each witness carries 'ok' or the error's name.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBKEYPATH_H
#define ENGINE_HOST_FIXTURE_IDBKEYPATH_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBKEYPATH[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var cfg = await (await fetch('/api/idbkp-cfg')).json();"
    "  var r = indexedDB.open('kp', 1);"
    "  r.onupgradeneeded = function () {"
    "    var db = r.result, got;"
    "    try { db.createObjectStore('s', { keyPath: cfg.kp }); got = 'ok'; } catch (e) { got = e.name; }"
    "    fetch('/api/idbkp-store?got=' + got);"
    "    try { db.createObjectStore('t').createIndex('i', cfg.ip); got = 'ok'; } catch (e) { got = e.name; }"
    "    fetch('/api/idbkp-index?got=' + got);"
    "    try { db.createObjectStore('u', { keyPath: cfg.ak, autoIncrement: true }); got = 'ok'; }"
    "    catch (e) { got = e.name; }"
    "    fetch('/api/idbkp-auto?got=' + got);"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
