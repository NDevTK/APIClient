/* THE RANDOM-CONSOLE DOCUMENT: console.log's format specifiers (Console §2.2 Formatter step 5) over
   Math.random(): %s converts through %String% and %d through %parseInt%, each over an unknown, and the
   converted value replaces its specifier. The witness fetch after the call carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MRCONSOLE_H
#define ENGINE_HOST_FIXTURE_MRCONSOLE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MRCONSOLE[] =
    "<!doctype html><html><body>"
    "<script>"
    "console.log('id %s n %d', Math.random(), 1e6 * Math.random());"
    "fetch('/api/mrc-logged');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
