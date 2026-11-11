/* THE COMPILE-LABEL-REST DOCUMENT: an indirect eval of a program declaring one function of 100,000 `if`
   statements, each a conditional jump to a label of its own. The parse and the code generation over that body
   (its prologue's scope linkage, label resolution, the stack-size walk) each charge more than a work quantum,
   so each must offer suspend points as it goes for no turn to hold the thread for a quantum. The function is
   not called, since its straight-line body would be a stretch of the interpreter's rather than the compile's;
   the eval's answer is its type, "function".
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COMPILELABELREST_H
#define ENGINE_HOST_FIXTURE_COMPILELABELREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COMPILELABELREST[] =
    "<!doctype html><html><body>"
    "<script>"
    "var clSrc = 'function clF(x) {' + 'if (x) x++;'.repeat(100000) + 'return x; } typeof clF';"
    "fetch('/api/cmp-label?r=' + (0, eval)(clSrc));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
