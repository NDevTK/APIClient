/* THE LAZY-LOAD DOCUMENT: a box below the first viewport is observed by an IntersectionObserver whose callback,
   once the box is visible, imports a chunk (TF_SERVED's `/chunk/lllazy.js`) and calls its init; a
   `visibilitychange` listener reports the page going hidden. The box intersects only after the person scrolls,
   and the page is hidden only when the person leaves it, so both arms hang on the victim's interaction and
   each must run beside the arm the first frame decides.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_LAZYLOAD_H
#define ENGINE_HOST_FIXTURE_LAZYLOAD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_LAZYLOAD[] =
    "<!doctype html><html><body>"
    "<div style='height: 3000px'></div><div id='llBox' style='height: 10px'></div>"
    "<script>"
    "var llIo = new IntersectionObserver(function llSeen(entries) {"
    "  if (entries[0].isIntersecting) {"
    "    fetch('/api/ll-visible');"
    "    llIo.disconnect();"
    "    import('/chunk/lllazy.js').then(function (m) { m.llInit(); });"
    "  } else {"
    "    fetch('/api/ll-offscreen');"
    "  }"
    "});"
    "llIo.observe(document.getElementById('llBox'));"
    "document.addEventListener('visibilitychange', function llVisibility() {"
    "  if (document.visibilityState === 'hidden') fetch('/api/ll-hidden');"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
