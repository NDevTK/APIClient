/* THE STATIC-MODULE DOCUMENT: a module script that statically imports /chunk/rxmod.js (test_forced.c's
   TF_SERVED), whose source holds a regular expression literal of 327681 pattern characters. ECMAScript
   §16.2.1.11 FinishLoadingImportedModule receives the source as a static request's answer and parses it, and
   that parse can hand the thread back like a parser-inserted script's, so the literal's compile rests inside
   it: the census counts rests while no RegExp constructor or compile runs, and the module's export (the
   matcher's answer) is what the address reports.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RXMODSTATIC_H
#define ENGINE_HOST_FIXTURE_RXMODSTATIC_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RXMODSTATIC[] =
    "<!doctype html><html><body>"
    "<script type=module>"
    "import { m } from '/chunk/rxmod.js';"
    "fetch('/api/rxs?m=' + m);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
