/* THE IDB-KEY-PATH-WALK DOCUMENT: Indexed Database §4.5 put into a store whose key path is a JSON reply field.
   §7.1 evaluate a key path on a value walks the path's identifiers, which the reply's field does not hold, so
   the key path names one of the value's members in each world: `id` (key 7), `name` (key "nm"),
   `name.length` (key 2), or none, where a store with no key generator refuses the put with a DataError. The
   witness carries the request's result or the error's name.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBKPWALK_H
#define ENGINE_HOST_FIXTURE_IDBKPWALK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBKPWALK[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var cfg = await (await fetch('/api/idbkw-cfg')).json();"
    "  var r = indexedDB.open('kw', 1);"
    "  r.onupgradeneeded = function () {"
    "    var st;"
    "    try { st = r.result.createObjectStore('s', { keyPath: cfg.kp }); }"
    "    catch (e) { fetch('/api/idbkw-made?got=' + e.name); return; }"
    "    try {"
    "      var q = st.put({ id: 7, name: 'nm' });"
    "      q.onsuccess = function () { fetch('/api/idbkw-put?got=' + q.result); };"
    "    } catch (e) { fetch('/api/idbkw-put?got=' + e.name); }"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
