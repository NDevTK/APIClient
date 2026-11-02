/* THE URL HREF DOCUMENT: URL §6.1's getters over a URL whose input is the query or the fragment.
   - `?u=` is resolved with `new URL(u, location.href)` and its `href` fetched wherever it points, so the
     attacker hosts the reply, which is compiled with `new Function`: the request's and the compiled text's
     sources are rooted in the page's own parse of `u=`, which the record's `href` derives from.
   - the fragment, after its `#`, is the query value of a same-origin `new URL('/api/uh-q?q=' + h, …).href`
     and of a `new Request('/api/uh-r?q=' + h)`: the page's own path decides both hosts, so neither is a
     request @S search.
   Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_URLHREF_H
#define ENGINE_HOST_FIXTURE_URLHREF_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_URLHREF[] =
    "<!doctype html><html><body>"
    "<script>"
    "function uhParam(name) {"
    "  var q = location.search, at = q.indexOf(name + '=');"
    "  return at < 0 ? null : decodeURIComponent(q.slice(at + name.length + 1).split('&')[0]);"
    "}"
    "(async function () {"
    "  var u = uhParam('u');"
    "  if (!u) return;"
    "  var href = new URL(u, location.href).href;"
    "  fetch('/api/uh-built');"
    "  new Function('x', await (await fetch(href)).text());"
    "})();"
    "(function () {"
    "  var h = location.hash.slice(1);"
    "  fetch(new URL('/api/uh-q?q=' + h, location.href).href);"
    "  fetch(new Request('/api/uh-r?q=' + h));"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
