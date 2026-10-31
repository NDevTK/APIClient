/* THE INDIRECT-EVAL DOCUMENT: an indirect eval of a program whose source holds a regular expression literal
   several work quanta long, built at run time. ECMAScript §19.2.1.1 PerformEval parses it, and that parse can
   hand the thread back like a parser-inserted script's, so the literal's compile rests inside it: the census
   counts rests while no RegExp constructor or compile runs, and the program's completion value (the matcher's
   answer) is what the address reports.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXEVAL_H
#define ENGINE_HOST_FIXTURE_RXEVAL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXEVAL[] =
    "<!doctype html><html><body>"
    "<script>"
    "var reN = 300000;"
    "var reS = 'var reR = /' + 'a'.repeat(reN) + 'b/;'"
    "  + 'reR.test(\\'' + 'a'.repeat(reN) + 'b\\') && !reR.test(\\'' + 'a'.repeat(reN) + '\\')'"
    "  + ' && reR.source.length === ' + (reN + 1);"
    "fetch('/api/rxe?m=' + (0, eval)(reS));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
