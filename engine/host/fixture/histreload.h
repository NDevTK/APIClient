/* THE HISTORY-RELOAD DOCUMENT: `history.go(0)` is HTML §7.2.5's delta traverse step 4, a reload of the
   document's node navigable, so the page's `navigate` listener sees a "reload" navigation. The listener cancels
   it, so the document stays and the run does not reload itself again.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_HISTRELOAD_H
#define ENGINE_HOST_FIXTURE_HISTRELOAD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_HISTRELOAD[] =
    "<!doctype html><html><body>"
    "<script>"
    "navigation.addEventListener('navigate', function (e) {"
    "  fetch('/api/hl-navigate?type=' + e.navigationType + '&cancelable=' + e.cancelable);"
    "  e.preventDefault();"
    "});"
    "history.go(0);"
    "fetch('/api/hl-after');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
