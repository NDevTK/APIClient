/* THE COMPILE-LOOKAHEAD-STRING-REST DOCUMENT: an indirect eval of a program that passes an object literal holding
   a string of 100,000 characters, the shape a bundle gives an SVG path (`g("path", {d: "…"})`). Before the parse
   can decide whether `{` opens an AssignmentPattern it scans ahead to the matching `}`, here over the whole
   string, charged a unit a byte; the compile must offer suspend points inside the string within that scan for no
   turn to hold the thread for a quantum. The eval's answer is the string's length, 100,000.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COMPILELOOKSTRREST_H
#define ENGINE_HOST_FIXTURE_COMPILELOOKSTRREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COMPILELOOKSTRREST[] =
    "<!doctype html><html><body>"
    "<script>"
    "var clsSrc = 'function clsF(o) { return o.d.length; } clsF({d: \"' + 'a'.repeat(100000) + '\"})';"
    "fetch('/api/cmp-lookstr?n=' + (0, eval)(clsSrc));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
