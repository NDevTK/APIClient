/* THE SPLIT-ARRAY DOCUMENT: ECMAScript §22.1.3.23 String.prototype.split returns an Array, and §23.1.3.21
   Array.prototype.map over it returns an Array, whatever String the receiver holds. The chain is the query-format
   sniff a recording snippet runs over a URL's query: `(e || "").replace("?", "").split("&").map(cb)`, then
   Array.isArray, `typeof` and `.some` over the result. `location.search` is an attacker source the page sets an
   example for first; `window.__tfSplitUnk`, narrowed by `typeof` to a String, is an absent global with no
   example. Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SPLITARR_H
#define ENGINE_HOST_FIXTURE_SPLITARR_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SPLITARR[] =
    "<!doctype html><html><body>"
    "<script>"
    "history.replaceState(null, '', '?saA=1&saB=2');"
    "function saLc(e, u) {"
    "  var r = (e || '').replace('?', '').split('&').map(function (x) {"
    "    fetch(u.cb + '?w=saCB');"
    "    return decodeURIComponent(x.split('=')[0]);"
    "  });"
    "  fetch(u.ty + '?w=' + (Array.isArray(r) ? 'saARRAY' : 'saNOTARRAY') +"
    "        '&k=' + (typeof r === 'object' ? 'saOBJECT' : 'saNOTOBJECT'));"
    "  var w = 'saNONE';"
    "  try { w = r.some(function (x, i) { return r.indexOf(x) !== i; }) ? 'saREPEAT' : 'saUNIQUE'; }"
    "  catch (ex) { w = 'saTHREW'; }"
    "  fetch(u.so + '?w=' + w);"
    "}"
    "saLc(location.search, { cb: '/api/saq-cb', ty: '/api/saq-type', so: '/api/saq-some' });"
    "var saU = window.__tfSplitUnk;"
    "if (typeof saU === 'string') saLc(saU, { cb: '/api/sag-cb', ty: '/api/sag-type', so: '/api/sag-some' });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
