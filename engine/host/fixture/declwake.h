/* THE DECLINED-WAKE DOCUMENT: members resting on one refused request while replies they do not hold arrive. The
   page POSTs to /api/decl/dwpost, which the host refuses, and then branches over three globals this session does
   not have, so several members hold that one record when its refusal lands; each forks its failure arm and its
   waiting arm rests on the refusal. The example's world then fetches a chain of twelve GETs the host serves, one
   after another. A reply wakes only the members whose register holds its record, so no waiting arm is stepped
   again by a link of the chain: each member rests on the refusal once, after its own chain, and those resting are
   the session's host-owed survivors. One of test_forced.c's exploring documents (TF_DOCS); it ends with
   TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DECLWAKE_H
#define ENGINE_HOST_FIXTURE_DECLWAKE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DECLWAKE[] =
    "<!doctype html><html><body>"
    "<script>"
    "fetch('/api/decl/dwpost', { method: 'POST', body: 'dw' });"
    "if (window.__tfDwGate) { if (window.__tfDwA) {} if (window.__tfDwB) {} }"
    " else (function dwNext(i) {"
    " if (i < 12) fetch('/api/dwchain?i=' + i).then(function () { dwNext(i + 1); }); })(0);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
