/* THE COMPILE-ASYNC-REST DOCUMENT: an indirect eval of a program that calls a function named `async` with an
   array literal of 100,001 elements. Before the parse can decide whether `async (` opens an async arrow's
   parameters it scans ahead to the matching `)`, here the whole literal, charged a unit a byte; the compile must
   offer suspend points inside that scan for no turn to hold the thread for a quantum. The eval's answer is the
   array's length, 100,001.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COMPILEASYNCREST_H
#define ENGINE_HOST_FIXTURE_COMPILEASYNCREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COMPILEASYNCREST[] =
    "<!doctype html><html><body>"
    "<script>"
    "var cyhSrc = 'function async(a) { return a.length; } async([' + '1,'.repeat(100000) + '0])';"
    "fetch('/api/cmp-async?n=' + (0, eval)(cyhSrc));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
