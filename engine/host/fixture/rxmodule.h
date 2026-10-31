/* THE DYNAMIC-MODULE DOCUMENT: a dynamic import of /chunk/rxmod.js (test_forced.c's TF_SERVED), whose source
   holds a regular expression literal of 327681 pattern characters. ECMAScript §13.3.10.3 ContinueDynamicImport
   receives the source and parses it into a module record, and that parse can hand the thread back like a
   parser-inserted script's, so the literal's compile rests inside it: the census counts rests while no RegExp
   constructor or compile runs, and the module's export (the matcher's answer) is what the address reports.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXMODULE_H
#define ENGINE_HOST_FIXTURE_RXMODULE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXMODULE[] =
    "<!doctype html><html><body>"
    "<script>"
    "import('/chunk/rxmod.js').then(function (ns) { fetch('/api/rxm?m=' + ns.m); },"
    " function (e) { fetch('/api/rxm?m=' + e.name); });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
