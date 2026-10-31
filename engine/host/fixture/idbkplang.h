/* THE IDB-KEY-PATH-LANGUAGE DOCUMENT: Indexed Database §4.4 createObjectStore with a key path read from a fetched
   JSON reply, followed by the page's own equalities on that key path. §2.5's valid key path forks the valid
   world and the SyntaxError world; in each, an equality against a string the world's answer excludes ("a..b"
   is no valid key path, "a.b" is one) has only its failing arm, while the one the world admits keeps both.
   The witness carries 'ok' or the error's name, then which equality held.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_IDBKPLANG_H
#define ENGINE_HOST_FIXTURE_IDBKPLANG_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_IDBKPLANG[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var cfg = await (await fetch('/api/idbkl-cfg')).json();"
    "  var r = indexedDB.open('kl', 1);"
    "  r.onupgradeneeded = function () {"
    "    var db = r.result, got;"
    "    try { db.createObjectStore('s', { keyPath: cfg.kp }); got = 'ok'; } catch (e) { got = e.name; }"
    "    var eq = cfg.kp === 'a..b' ? 'dots' : cfg.kp === 'a.b' ? 'ab' : 'other';"
    "    fetch('/api/idbkl-eq?got=' + got + '-' + eq);"
    "  };"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
