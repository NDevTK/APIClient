/* THE JOIN-COMPOSITION DOCUMENT: §23.1.3.18 Array.prototype.join over an array holding one unknown (an injected
   global the document does not spell, which solver/absent.c mints) is string-concatenation of its pieces, so
   its address composes exactly as the same pieces joined by `+` do, every known piece kept. String(array) and a
   template literal reach the same join through Array.prototype.toString, and an all-constant join stays
   concrete.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_JOINCOMPOSE_H
#define ENGINE_HOST_FIXTURE_JOINCOMPOSE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_JOINCOMPOSE[] =
    "<!doctype html><html><body>"
    "<script>"
    "var jcU = window.__tfJoinU;"
    "fetch('/api/jc/join/' + ['aa', jcU, 'bb'].join('&'));"
    "fetch('/api/jc/plus/' + ('aa' + '&' + jcU + '&' + 'bb'));"
    "fetch('/api/jc/tostr/' + String(['aa', jcU, 'bb']));"
    "fetch('/api/jc/tpl/' + `${['aa', jcU, 'bb']}`);"
    "fetch('/api/jc/const/' + ['aa', 'cc', 'bb'].join('&'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
