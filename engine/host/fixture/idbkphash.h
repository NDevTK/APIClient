/* THE IDB-KEY-PATH-HASH DOCUMENT: Indexed Database §4.5 put into a store whose key path is location.hash's
   tail. The tail is a String, so §3.2.25 gives it the string arm alone, and §7.1 evaluate a key path on a value
   names one of the value's members in each world: `id` (key 7), `name` (key "nm"), `name.length` (key 2), or
   none, where a store with no key generator refuses the put with a DataError. The witness carries the request's
   result or the error's name.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBKPHASH_H
#define ENGINE_HOST_FIXTURE_IDBKPHASH_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBKPHASH[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  var r = indexedDB.open('kh', 1);"
    "  r.onupgradeneeded = function () {"
    "    var st;"
    "    try { st = r.result.createObjectStore('s', { keyPath: location.hash.slice(1) }); }"
    "    catch (e) { fetch('/api/idbkh-made?got=' + e.name); return; }"
    "    try {"
    "      var q = st.put({ id: 7, name: 'nm' });"
    "      q.onsuccess = function () { fetch('/api/idbkh-put?got=' + q.result); };"
    "    } catch (e) { fetch('/api/idbkh-put?got=' + e.name); }"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
