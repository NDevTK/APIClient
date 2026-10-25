/* THE REQUEST DOCUMENT: Fetch §5.4's Request constructor over an address the query names, the shape a fetch
   interceptor wraps every call in. Each statement builds `new Request(url)` from a query parameter, fetches it,
   and compiles the reply with `new Function`.
   - `?src=` is wrapped and fetched wherever it points, so the attacker hosts the reply: the compiled text is
     attacker input rooted in the query.
   - `?page=` (falling back to the seed, TF_SERVED's `/files/fpseed.txt`) is fetched only when it resolves to
     this document's own origin, by the page's own `new URL(url, location.href).origin` test, which the
     request's URL shares: its sink must carry no source rooted in the query.
   Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_REQUESTUNK_H
#define ENGINE_HOST_FIXTURE_REQUESTUNK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REQUESTUNK[] =
    "<!doctype html><html><body>"
    "<script>"
    "function rquParam(name) {"
    "  var q = location.search, at = q.indexOf(name + '=');"
    "  return at < 0 ? null : decodeURIComponent(q.slice(at + name.length + 1).split('&')[0]);"
    "}"
    "(async function () {"
    "  var url = rquParam('src');"
    "  if (!url) return;"
    "  var req = new Request(url);"
    "  fetch('/api/rqu-built');"
    "  new Function('x', await (await fetch(req)).text());"
    "})();"
    "(async function () {"
    "  var url = rquParam('page') || '/files/fpseed.txt';"
    "  if (new URL(url, location.href).origin !== location.origin) { fetch('/api/rqu-refused'); return; }"
    "  new Function('x', await (await fetch(new Request(url))).text());"
    "  fetch('/api/rqu-pinned');"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
