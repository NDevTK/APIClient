/* THE REALM SELL CLOSE DOCUMENT: realmsell.h's shape with the popup closed before its holder parks, so HTML
   §7.5.10's destroy has released the navigable's Document and only the holder's delta names the realm. Served with a small RAM pressure share (its TF_DOCS
   row), so ordinary pressure sells the parked holder while the session is live, owes a collection after the
   sale, and that collection frees the realm. One of test_forced.c's exploring documents. It ends with
   TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_REALMSELLCLOSE_H
#define ENGINE_HOST_FIXTURE_REALMSELLCLOSE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REALMSELLCLOSE[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var _rcw = null, _rccfg = { holder: state.closer };"
    "(async function () {"
    /* Absent application state forks the two arms, and both run (the assumed one ranks behind, by its cost). The
       holder arm opens a same-origin popup, reads through its proxy (which materializes the popup's realm in this
       arm) and closes it, so HTML §7.2.2.1's close and §7.5.10's destroy release the navigable's hold on the
       Document. The write of `_rcw` keeps the proxy in this arm's delta and nowhere in the baseline, so selling
       this arm leaves the realm named by nothing. */
    " if (_rccfg.holder) {"
    "  _rcw = open(); _rcw.document.title; _rcw.close();"
    /* §7.2.2.1 step 6.2 queues the task that definitely closes the traversable; two timer tasks later it has run,
       so the destroy has released the Document before this arm parks. */
    "  await new Promise(function (r) { setTimeout(r, 0); });"
    "  await new Promise(function (r) { setTimeout(r, 0); });"
    "  await fetch('/api/rsclose?arm=held');"
    "  fetch('/api/rsclose?arm=held-resumed');"
    " } else {"
    /* The other arm grows the heap in rounds, awaiting a reply between them so the holder arm runs and parks in
       between; the footprint keeps passing the share's mark, so pressure opens episodes after the holder has
       parked and sells it. */
    /* It starts after four timer tasks, so the holder's close has definitely closed the popup before the growth
       that makes pressure sell. */
    "  for (var t = 0; t < 4; t++) await new Promise(function (r) { setTimeout(r, 0); });"
    "  var a = [];"
    "  for (var k = 0; k < 40; k++) {"
    "   for (var i = 0; i < 20000; i++) a.push({ i: i, s: 'v' + k + '.' + i });"
    "   await fetch('/api/rsclose?arm=other&k=' + k);"
    "  }"
    "  fetch('/api/rsclose?arm=other-done&n=' + a.length);"
    " }"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
