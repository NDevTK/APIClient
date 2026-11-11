/* THE COMPILE-REGEXP-REST DOCUMENT: an indirect eval of a program whose one regular expression literal holds a
   200,001-character pattern. The literal's scan and its pattern's compile are each charged a unit a byte, several
   work quanta for this token, so the compile must offer a suspend point inside the token and inside the pattern
   for no turn to hold the thread for a quantum. The eval's answer is the literal's test against "ab" (false).
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COMPILERE_REST_H
#define ENGINE_HOST_FIXTURE_COMPILERE_REST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COMPILERE_REST[] =
    "<!doctype html><html><body>"
    "<script>"
    "var crSrc = 'var crR = /' + 'a'.repeat(200000) + 'b/; crR.test(\"ab\")';"
    "fetch('/api/cr-done?m=' + (0, eval)(crSrc));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
