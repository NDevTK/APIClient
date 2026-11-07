/* THE CROSS-DOMAIN HOST DOCUMENT: an anchor's protocol and host compared with those of an anchor holding the
   document's own `location.href`, as a request library decides whether a URL is cross-domain (the href written
   back through `a.href` once, as the library does). The query and the fragment are unknown input in every
   operand, and the page's own bytes decide the host wherever they reach past the authority.
   - `/api/xd-path` and `/api/xd-item/<query>` are paths the page wrote, so both hosts are the document's and the
     comparison has one outcome: only the same-host arm runs.
   - `new URL('/api/xd-url/' + fragment, location.href).host` against `location.host` is the same question asked
     of the URL interface.
   - an href that is the fragment itself leaves the host to the attacker, so both arms run.
   Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_XDHOST_H
#define ENGINE_HOST_FIXTURE_XDHOST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_XDHOST[] =
    "<!doctype html><html><body>"
    "<script>"
    "var xdOrigin = document.createElement('a');"
    "xdOrigin.href = location.href;"
    "function xdCross(url) {"
    "  var a = document.createElement('a');"
    "  a.href = url;"
    "  a.href = a.href;"
    "  return xdOrigin.protocol + '//' + xdOrigin.host !== a.protocol + '//' + a.host;"
    "}"
    "fetch(xdCross('/api/xd-path') ? '/api/xd-cross-a' : '/api/xd-same-a');"
    "fetch(xdCross('/api/xd-item/' + encodeURIComponent(location.search.slice(1))) ? '/api/xd-cross-b'"
    "                                                                               : '/api/xd-same-b');"
    "fetch(new URL('/api/xd-url/' + location.hash.slice(1), location.href).host === location.host"
    "      ? '/api/xd-same-c' : '/api/xd-cross-c');"
    "fetch(xdCross(location.hash.slice(1)) ? '/api/xd-open-cross' : '/api/xd-open-same');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
