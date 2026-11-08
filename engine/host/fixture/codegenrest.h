/* THE CODEGEN-REST DOCUMENT: one classic script holding a function of 20,000 statements and 3,000 nested function
   expressions, so the code generation that follows the parse (quickjs.c's js_create_function: variable and label
   resolution, the peephole pass and the stack-size walk over every function) is several work quanta long. Each
   instruction a pass processes is charged as host work and the passes rest between instructions, so the census
   counts rests and both bodies still compute what they state: the address reports the long body's sum and how
   many nested functions ran.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CODEGENREST_H
#define ENGINE_HOST_FIXTURE_CODEGENREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define CGR_S10 "a=a+1|0;a=a+1|0;a=a+1|0;a=a+1|0;a=a+1|0;a=a+1|0;a=a+1|0;a=a+1|0;a=a+1|0;a=a+1|0;"
#define CGR_S100 CGR_S10 CGR_S10 CGR_S10 CGR_S10 CGR_S10 CGR_S10 CGR_S10 CGR_S10 CGR_S10 CGR_S10
#define CGR_S1000 CGR_S100 CGR_S100 CGR_S100 CGR_S100 CGR_S100 CGR_S100 CGR_S100 CGR_S100 CGR_S100 CGR_S100
#define CGR_S10000 CGR_S1000 CGR_S1000 CGR_S1000 CGR_S1000 CGR_S1000 CGR_S1000 CGR_S1000 CGR_S1000 CGR_S1000 \
                   CGR_S1000
#define CGR_F1 "(function(){n=n+1|0})();"
#define CGR_F10 CGR_F1 CGR_F1 CGR_F1 CGR_F1 CGR_F1 CGR_F1 CGR_F1 CGR_F1 CGR_F1 CGR_F1
#define CGR_F100 CGR_F10 CGR_F10 CGR_F10 CGR_F10 CGR_F10 CGR_F10 CGR_F10 CGR_F10 CGR_F10 CGR_F10
#define CGR_F1000 CGR_F100 CGR_F100 CGR_F100 CGR_F100 CGR_F100 CGR_F100 CGR_F100 CGR_F100 CGR_F100 CGR_F100

static const char HTML_CODEGENREST[] =
    "<!doctype html><html><body>"
    "<script>"
    "var n = 0;"
    "function cgrBody(){var a=0;" CGR_S10000 CGR_S10000 "return a}"
    "var cgrA = cgrBody();"
    CGR_F1000 CGR_F1000 CGR_F1000
    "fetch('/api/cgr-done?a=' + cgrA + '&n=' + n);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
