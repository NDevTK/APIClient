/* THE REPLY-TWICE DOCUMENT: the page fetches an address and is answered; then one arm of a fork parks on the
   same address again while the other arm grows the heap until RAM pressure sells the parked arm (the document
   is served with a small share, its TF_DOCS row). The fixture owes the second park's answer until the growth
   ends (test_forced.c's tf_replytwice_hold), so the sale drops the pair's last park while the zone owes it.
   One of test_forced.c's exploring documents. It ends with TF_CANVAS_INK, which test_forced.c defines before
   including it. */
#ifndef ENGINE_HOST_FIXTURE_REPLYTWICE_H
#define ENGINE_HOST_FIXTURE_REPLYTWICE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REPLYTWICE[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var _r2cfg = { hold: state.hold };"
    "(async function () {"
    /* The boot flow's park on the pair, answered at once. */
    " await fetch('/api/replytwice');"
    /* Absent application state forks the two arms, and the assumed one runs first. It parks on the same request
       again at once: a fresh park of a pair the zone has answered once, whose answer the zone owes while the
       other arm runs. */
    " if (_r2cfg.hold) {"
    "  await fetch('/api/replytwice');"
    "  fetch('/api/replytwice-resumed');"
    " } else {"
    /* The other arm grows the heap in rounds, awaiting a reply between them; the footprint keeps passing the
       share's mark, so pressure sells the parked holder. */
    "  var a = [];"
    "  for (var k = 0; k < 40; k++) {"
    "   for (var i = 0; i < 20000; i++) a.push({ i: i, s: 'v' + k + '.' + i });"
    "   await fetch('/api/replytwice-other?k=' + k);"
    "  }"
    "  fetch('/api/replytwice-other-done?n=' + a.length);"
    " }"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
