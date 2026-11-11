/* THE COMPILE-STRING-REST DOCUMENT: an indirect eval of a program whose one string literal holds 1,000,000
   characters. The literal's scan is charged a unit a byte at the script tokenizer's rate, many work quanta for
   this token, so the compile must offer a suspend point inside the token for no turn to hold the thread for a
   quantum. The eval's answer is the literal's length, reported once the program has run.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COMPILESTRREST_H
#define ENGINE_HOST_FIXTURE_COMPILESTRREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COMPILESTRREST[] =
    "<!doctype html><html><body>"
    "<script>"
    "var csSrc = 'var csS = \"' + 'a'.repeat(1000000) + '\"; csS.length';"
    "fetch('/api/cmp-str?n=' + (0, eval)(csSrc));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
