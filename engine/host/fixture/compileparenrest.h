/* THE COMPILE-PAREN-REST DOCUMENT: an indirect eval of a program that assigns one parenthesized function
   expression of 50,000 statements, the shape a bundler gives a whole bundle. Before the parse can decide whether
   `(` opens an arrow function's parameters it scans ahead to the matching `)`, here the whole body, charged a
   unit a byte; the compile must offer suspend points inside that scan for no turn to hold the thread for a
   quantum. The function is not called, since its straight-line body would be a stretch of the interpreter's;
   the eval's answer is its type, "function".
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COMPILEPARENREST_H
#define ENGINE_HOST_FIXTURE_COMPILEPARENREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COMPILEPARENREST[] =
    "<!doctype html><html><body>"
    "<script>"
    "var cpSrc = 'var cpF; cpF = (function () { var cpX = 0;' + 'cpX++;'.repeat(50000) + 'return cpX; }); typeof cpF';"
    "fetch('/api/cmp-paren?r=' + (0, eval)(cpSrc));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
