/* THE DATE-PARSE-COERCION DOCUMENT: ECMAScript §21.4.3.2 Date.parse ( string ) given an object whose toString is
   the page's own code. The function applies ToString to its argument, so the page's toString runs as part of
   the call and the date it answers is what is parsed. The witness carries the time value and how many times
   the page's loop ran.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DATEPARSECOERCE_H
#define ENGINE_HOST_FIXTURE_DATEPARSECOERCE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DATEPARSECOERCE[] =
    "<!doctype html><html><body>"
    "<script>"
    "var dpN = 0;"
    "var dpT = Date.parse({ toString: function () { for (var i = 0; i < 3; i++) dpN++; "
    "return '1970-01-02T00:00:00Z'; } });"
    "fetch('/api/dpc-parse?got=' + dpT + '-' + dpN);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
