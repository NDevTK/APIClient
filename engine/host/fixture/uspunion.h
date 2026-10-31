/* THE URLSEARCHPARAMS-INIT DOCUMENT: URL §6.2's init is Web IDL §3.2.25's
   `(sequence<sequence<USVString>> or record<USVString, USVString> or USVString)`, here handed fields of a fetched
   JSON reply, which reach the constructor as unknowns whose examples are the served values: a string, an object
   and an array of pairs. Each field's type facet admits every arm, so each construction forks them, and the
   world its example takes reads the pair back with get().
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_USPUNION_H
#define ENGINE_HOST_FIXTURE_USPUNION_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_USPUNION[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var c = await (await fetch('/api/uspi')).json();"
    "  fetch('/api/uspq?v=' + new URLSearchParams(c.q).get('a'));"
    "  fetch('/api/uspo?v=' + new URLSearchParams(c.o).get('b'));"
    "  fetch('/api/uspl?v=' + new URLSearchParams(c.l).get('c'));"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
