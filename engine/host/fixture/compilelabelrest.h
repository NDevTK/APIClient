/* THE COMPILE-LABEL-REST DOCUMENT: an indirect eval of a program declaring one function of 100,000 `if`
   statements, each a conditional jump to a label of its own, then calling it. The parse and the code generation
   passes over that body (label resolution, the stack-size walk) each charge more than a work quantum, so each
   must offer suspend points as it goes for no turn to hold the thread for a quantum. The eval's answer is the
   call's result, 100,001.
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
    "var clSrc = 'function clF(x) {' + 'if (x) x++;'.repeat(100000) + 'return x; } clF(1)';"
    "fetch('/api/cl-done?r=' + (0, eval)(clSrc));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
