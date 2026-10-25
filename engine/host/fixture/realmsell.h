/* THE REALM SELL DOCUMENT: a popup opened inside one arm of a fork and left open, named by nothing but that arm,
   which then parks on a reply while the other arm keeps working. The popup's traversable is part of the holder's
   world, so once ordinary RAM pressure sells the holder (the document is served with a small share, its TF_DOCS
   row) nothing names the realm, and the collection pressure owes after the sale frees it. One of test_forced.c's
   exploring documents. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_REALMSELL_H
#define ENGINE_HOST_FIXTURE_REALMSELL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REALMSELL[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var _rsw = null, _rscfg = { holder: state.holder };"
    "(async function () {"
    /* Absent application state forks the two arms, and both run (the assumed one ranks behind, by its cost). The
       holder arm opens a same-origin popup and reads through its proxy, which materializes the popup's realm in
       this arm; the write of `_rsw` puts the proxy in this arm's delta and nowhere in the baseline, so selling
       this arm leaves the realm named by nothing. */
    " if (_rscfg.holder) {"
    "  _rsw = open(); _rsw.document.title;"
    "  await fetch('/api/realmsell?arm=held');"
    "  fetch('/api/realmsell?arm=held-resumed');"
    " } else {"
    /* The other arm grows the heap in rounds, awaiting a reply between them so the holder arm runs and parks in
       between; the footprint keeps passing the share's mark, so pressure opens episodes after the holder has
       parked and sells it. */
    "  var a = [];"
    "  for (var k = 0; k < 40; k++) {"
    "   for (var i = 0; i < 20000; i++) a.push({ i: i, s: 'v' + k + '.' + i });"
    "   await fetch('/api/realmsell?arm=other&k=' + k);"
    "  }"
    "  fetch('/api/realmsell?arm=other-done&n=' + a.length);"
    " }"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
