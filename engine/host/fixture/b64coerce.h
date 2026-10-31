/* THE BASE64-COERCION DOCUMENT: HTML §8.3 Base64 utility methods' atob and btoa, each given an object whose
   toString is the page's own code. Web IDL converts the DOMString argument with ToString, so the page's toString
   runs as part of the call and its answer is what is decoded or encoded. Each witness carries the result and
   how many times the page's loop ran.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_B64COERCE_H
#define ENGINE_HOST_FIXTURE_B64COERCE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_B64COERCE[] =
    "<!doctype html><html><body>"
    "<script>"
    "var bcN = 0;"
    "var bcA = atob({ toString: function () { for (var i = 0; i < 3; i++) bcN++; return 'YWI='; } });"
    "fetch('/api/b64c-atob?got=' + bcA + '-' + bcN);"
    "var bcB = btoa({ toString: function () { for (var i = 0; i < 3; i++) bcN++; return 'ab'; } });"
    "fetch('/api/b64c-btoa?got=' + bcB + '-' + bcN);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
