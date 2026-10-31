/* THE FUNCTION-CONSTRUCTOR DOCUMENT: a Function constructor whose body holds a regular expression literal several
   work quanta long, built at run time. ECMAScript §20.2.1.1.1 CreateDynamicFunction parses it, and that parse
   can hand the thread back like a parser-inserted script's, so the literal's compile rests inside it: the
   census counts rests while no RegExp constructor or compile runs, and the function's result (the matcher's
   answer) is what the address reports.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXFUNC_H
#define ENGINE_HOST_FIXTURE_RXFUNC_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXFUNC[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rfN = 300000;"
    "var rfS = 'var rfR = /' + 'a'.repeat(rfN) + 'b/;'"
    "  + 'rfR.test(\\'' + 'a'.repeat(rfN) + 'b\\') && !rfR.test(\\'' + 'a'.repeat(rfN) + '\\')'"
    "  + ' && rfR.source.length === ' + (rfN + 1);"
    "fetch('/api/rxf?m=' + new Function(rfS.replace(/;/, ';return '))());"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
