/* THE DIRECT-EVAL DOCUMENT: two direct evals inside a function, `eval(src)` and `eval(...[src])`, of a program
   whose source holds a regular expression literal several work quanta long and reads the caller's own local.
   ECMAScript §19.2.1.1 PerformEval parses it in the caller's scope, and that parse can hand the thread back
   like a parser-inserted script's, so the literal's compile rests inside it and the eval opcode re-executes
   over the source it already checked. The witness carries each program's answer, which also needs the
   caller's local, and the census counts rests while no RegExp constructor or compile runs.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXDEVAL_H
#define ENGINE_HOST_FIXTURE_RXDEVAL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXDEVAL[] =
    "<!doctype html><html><body>"
    "<script>"
    "(function () {"
    "  var rdLocal = 7, rdN = 300000;"
    "  var rdS = 'var rdR = /' + 'a'.repeat(rdN) + 'b/;'"
    "    + 'rdR.test(\"' + 'a'.repeat(rdN) + 'b\") && !rdR.test(\"' + 'a'.repeat(rdN) + '\")'"
    "    + ' && rdR.source.length === ' + (rdN + 1) + ' && rdLocal === 7';"
    "  fetch('/api/rxd-call?m=' + eval(rdS));"
    "  fetch('/api/rxd-spread?m=' + eval(...[rdS]));"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
