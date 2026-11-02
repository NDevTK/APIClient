/* THE FRAMED-MARGIN DOCUMENT: rooted as a cross-origin child whose container frame element, in the peer,
   carries marginheight=20 and no marginwidth (test_forced.c's TF_DOCS row states its margins). HTML §15.3.2
   "The page" maps the container's marginheight to the body's top and bottom margins and leaves the left and
   right at 8px; the body states neither attribute itself, so the container is the source read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRAMEMARGIN_H
#define ENGINE_HOST_FIXTURE_FRAMEMARGIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FRAMEMARGIN[] =
    "<!doctype html><html><body>"
    "<script>"
    "var s = getComputedStyle(document.body);"
    "fetch('/api/fm-margins?top=' + s.marginTop + '&left=' + s.marginLeft);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
