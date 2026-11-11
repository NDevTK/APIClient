/* THE COMPILE-ARRAY-REST DOCUMENT: an indirect eval of a program that assigns an array literal of 100,000
   elements, the shape a bundle gives a data table. Before the parse can decide whether `[` opens an
   AssignmentPattern it scans ahead to the matching `]`, here the whole literal, charged a unit a byte; the
   compile must offer suspend points inside that scan for no turn to hold the thread for a quantum. The eval's
   answer is the array's length, 100,000.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COMPILEARRAYREST_H
#define ENGINE_HOST_FIXTURE_COMPILEARRAYREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COMPILEARRAYREST[] =
    "<!doctype html><html><body>"
    "<script>"
    "var caSrc = 'var caA; caA = [' + '1,'.repeat(100000) + ']; caA.length';"
    "fetch('/api/cmp-array?n=' + (0, eval)(caSrc));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
