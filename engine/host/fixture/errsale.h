/* THE REPORT-SALE DOCUMENT: `reportError` runs HTML §8.1.4.6 "report an exception", which sets the global's
   in error reporting mode (step 6.1) and fires `error` (step 6.2). The listener forks on absent application
   state and then grows the heap, so whichever arm is parked is parked inside step 6.2's dispatch with the mode
   set in its own world, and RAM pressure sells it (the document is served with a small share, its TF_DOCS
   row). The surviving arm's listener reports again: its global is still in error reporting mode, so step 6
   fires nothing and the listener runs once.
   One of test_forced.c's exploring documents. It ends with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_ERRSALE_H
#define ENGINE_HOST_FIXTURE_ERRSALE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ERRSALE[] =
    "<!doctype html><html><body>"
    "<script>"
    "var _escfg = { hold: state.esHold };"
    "var esSeen = 0, esMode = 'none';"
    "addEventListener('error', function (e) {"
    "  esSeen++;"
    "  esMode = _escfg.hold ? 'held' : 'free';"
    "  var a = [];"
    "  for (var i = 0; i < 400000; i++) a.push({ i: i, s: 'v' + i });"
    "  reportError(new Error('esInner'));"
    "  e.preventDefault();"
    "});"
    "reportError(new Error('esOuter'));"
    "fetch('/api/essell?m=' + esMode + '&c=' + esSeen);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
