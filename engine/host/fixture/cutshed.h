/* THE CUT-SHED DOCUMENT: the page reads `location.hash` at boot, so a snapshot is held before that first read
   (solver/source_cut.h), and then one arm of a fork grows the heap while the other writes the fragment to markup.
   The document is served with a small RAM share (its TF_DOCS row), so the allocator's refusals reach the pager
   while the snapshot is held, and the pager answers them by shedding it before it sells any member.
   One of test_forced.c's exploring documents. It ends with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_CUTSHED_H
#define ENGINE_HOST_FIXTURE_CUTSHED_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CUTSHED[] =
    "<!doctype html><html><body>"
    "<div id='csOut'></div>"
    "<script>"
    "var csH = location.hash;"
    "var _cscfg = { grow: state.grow };"
    "(async function () {"
    " if (_cscfg.grow) {"
    "  var a = [];"
    "  for (var k = 0; k < 40; k++) {"
    "   for (var i = 0; i < 20000; i++) a.push({ i: i, s: 'v' + k + '.' + i });"
    "   await fetch('/api/cutshed-other?k=' + k);"
    "  }"
    " } else {"
    "  document.getElementById('csOut').innerHTML = decodeURIComponent(csH.slice(1));"
    " }"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
