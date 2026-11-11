/* The sale-collection document: fixture/realmsell.h's two arms over a live set of 300000 objects built before
   the fork, so the runtime is large when ordinary RAM pressure (a small share, its TF_DOCS row) sells the arm
   that alone names a popup's realm. What the sale leaves is that realm, a cycle; the heap around it is shared
   and is none of the sold arm's. One of test_forced.c's exploring documents. It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GCSALE_H
#define ENGINE_HOST_FIXTURE_GCSALE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GCSALE[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var _gsKeep = [];"
    "for (var c = 0; c < 300; c++) {"
    "  var _gsRow = [];"
    "  for (var i = 0; i < 1000; i++) _gsRow.push({ i: i });"
    "  _gsKeep.push(_gsRow);"
    "}"
    "var _gsw = null, _gscfg = { holder: state.holder };"
    "(async function () {"
    " if (_gscfg.holder) {"
    "  _gsw = open(); _gsw.document.title;"
    "  await fetch('/api/gcsale?arm=held');"
    "  fetch('/api/gcsale?arm=held-resumed');"
    " } else {"
    "  var a = [];"
    "  for (var k = 0; k < 40; k++) {"
    "   for (var i = 0; i < 20000; i++) a.push({ i: i, s: 'v' + k + '.' + i });"
    "   await fetch('/api/gcsale?arm=other&k=' + k);"
    "  }"
    "  fetch('/api/gcsale?arm=other-done&n=' + a.length);"
    " }"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
