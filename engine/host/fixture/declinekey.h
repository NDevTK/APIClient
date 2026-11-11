/* THE DECLINE-KEY DOCUMENT: two timelines ask one request (one PendRequestKey), one on an assumed arm and one on
   the page's own run, while the zone's answer to the first is still owed.
   /api/dkey: the assumed arm parks at once and its line, stating `forced`, is judged and refused a payment later
   (test_forced.c's tf_dk_hold), after the page's own run has parked on the same request behind /api/dkey-pre.
   The refusal answers the line the zone judged, so the own run's park is listed alone and sent.
   /api/dkrev: the reverse order. The own run parks at once and its line is sent, the reply owed until the
   assumed arm has parked on the same request behind /api/dkrev-pre; the one reply fills both parks.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DECLINEKEY_H
#define ENGINE_HOST_FIXTURE_DECLINEKEY_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DECLINEKEY[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var _dkcfg = { first: state.dkfirst, rev: state.dkrev };"
    "(async function () {"
    " if (_dkcfg.first) {"
    "  try { await fetch('/api/dkey'); fetch('/api/dkey-forced-got'); }"
    "  catch (e) { fetch('/api/dkey-forced-fail'); }"
    " } else {"
    "  await fetch('/api/dkey-pre');"
    "  try { await fetch('/api/dkey'); fetch('/api/dkey-own-got'); }"
    "  catch (e) { fetch('/api/dkey-own-fail'); }"
    " }"
    "})();"
    "(async function () {"
    " if (_dkcfg.rev) {"
    "  await fetch('/api/dkrev-pre');"
    "  await fetch('/api/dkrev');"
    "  fetch('/api/dkrev-forced-got');"
    " } else {"
    "  await fetch('/api/dkrev');"
    "  fetch('/api/dkrev-own-got');"
    " }"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
