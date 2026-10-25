/* THE IDB-SCOPE-OWN DOCUMENT: Indexed Database §4.4 transaction(storeNames) over the result of a page-defined
   String.prototype.slice, which returns JSON.parse of the hash. The callee is the page's function, so no
   definition fixes the result's type: a hash of `["alpha","beta"]` makes it a sequence and `"alpha"` a string,
   and Web IDL §3.2.25 runs both arms. The witness carries the scope's names joined by '_', or the error's name.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBSCOPEOWN_H
#define ENGINE_HOST_FIXTURE_IDBSCOPEOWN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBSCOPEOWN[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  String.prototype.slice = function (n) { return JSON.parse(this.substring(n)); };"
    "  var r = indexedDB.open('so', 1);"
    "  r.onupgradeneeded = function () {"
    "    r.result.createObjectStore('alpha');"
    "    r.result.createObjectStore('beta');"
    "  };"
    "  r.onsuccess = function () {"
    "    var got;"
    "    try {"
    "      var t = r.result.transaction(location.hash.slice(1)).objectStoreNames, s = [];"
    "      for (var i = 0; i < t.length; i++) s.push(t[i]);"
    "      got = s.join('_');"
    "    } catch (e) { got = e.name; }"
    "    fetch('/api/idbso-tx?got=' + got);"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
