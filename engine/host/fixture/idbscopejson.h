/* THE IDB-SCOPE-JSON DOCUMENT: Indexed Database §4.4 transaction(storeNames) where storeNames is a field of a
   fetched JSON reply. Web IDL §3.2.25's `(DOMString or sequence<DOMString>)` over that unknown is two worlds: a
   string (one name) and an Object with @@iterator (a sequence of names), so one arm scopes the transaction to
   both stores 'alpha' and 'beta' and another to an empty sequence (InvalidAccessError). location.hash.slice(1)
   is a string, so the second transaction has only the string arm. Each witness carries the scope's names
   joined by '_', or the error's name.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBSCOPEJSON_H
#define ENGINE_HOST_FIXTURE_IDBSCOPEJSON_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBSCOPEJSON[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var cfg = await (await fetch('/api/idbsj-cfg')).json();"
    "  var r = indexedDB.open('sj', 1);"
    "  r.onupgradeneeded = function () {"
    "    r.result.createObjectStore('alpha');"
    "    r.result.createObjectStore('beta');"
    "  };"
    "  r.onsuccess = function () {"
    "    function scope(x) {"
    "      try {"
    "        var t = r.result.transaction(x).objectStoreNames, s = [];"
    "        for (var i = 0; i < t.length; i++) s.push(t[i]);"
    "        return s.join('_');"
    "      } catch (e) { return e.name; }"
    "    }"
    "    fetch('/api/idbsj-json?got=' + scope(cfg.stores));"
    "    fetch('/api/idbsj-hash?got=' + scope(location.hash.slice(1)));"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
