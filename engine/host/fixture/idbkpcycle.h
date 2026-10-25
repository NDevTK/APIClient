/* THE IDB-KEY-PATH-CYCLE DOCUMENT: Indexed Database §4.5 put of a value whose `self` member is the value itself,
   into a store whose key path is location.hash's tail. §7.1 evaluate a key path on a value names one member in
   each world, and the cycle makes the paths through `self` endless, so the world where the key path names no
   member (a DataError, since the store has no key generator) is reached only if each level of the value asks
   it. The witness carries the request's result or the error's name, marked `-none` where this engine's message
   says the value has nothing at the key path: the "" world's DataError (the value itself is not a key) is the
   same name for a different step.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBKPCYCLE_H
#define ENGINE_HOST_FIXTURE_IDBKPCYCLE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBKPCYCLE[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  var r = indexedDB.open('kc', 1);"
    "  r.onupgradeneeded = function () {"
    "    var st, v = { id: 7 };"
    "    v.self = v;"
    "    try { st = r.result.createObjectStore('s', { keyPath: location.hash.slice(1) }); }"
    "    catch (e) { fetch('/api/idbkc-made?got=' + e.name); return; }"
    "    try {"
    "      var q = st.put(v);"
    "      q.onsuccess = function () { fetch('/api/idbkc-put?got=' + q.result); };"
    "    } catch (e) { fetch('/api/idbkc-put?got=' + e.name + (/no value/.test(e.message) ? '-none' : '')); }"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
