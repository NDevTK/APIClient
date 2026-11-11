/* The reply-record documents. `replyrec`: a page that asks for one reply and reports its length in a second
   request, so the store records two answered sends and the page's flow trails the first before it reads it.
   `replynone`: a page that asks for nothing, the control. Two of test_forced.c's exploring documents
   (TF_DOCS); each ends with TF_CANVAS_INK, which test_forced.c defines before including this file. */
#ifndef ENGINE_HOST_FIXTURE_REPLYRECORD_H
#define ENGINE_HOST_FIXTURE_REPLYRECORD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REPLYREC[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rrTrail = 1;"
    "fetch('/api/rr-owed').then(function (r) { return r.text(); })"
    "  .then(function (t) { fetch('/api/rr-done?b=' + t.length); });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_REPLYNONE[] =
    "<!doctype html><html><body>"
    "<script>var rrNone = 1;</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
