/* THE UNKNOWN ANIMATION FRAME HANDLE DOCUMENT: Bootstrap's shape, cancelAnimationFrame over a handle computed from the
   URL. Two callbacks are registered, then cancelAnimationFrame(location.hash.length), then a third callback that
   reports which of the first two ran. HTML §8.12's step 3 tells apart one world per handle the map holds plus one in
   which the handle names none, so the report reaches three values: "1-2", "2" and "1".
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RAFCANCEL_H
#define ENGINE_HOST_FIXTURE_RAFCANCEL_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RAFCANCEL[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rfRan = [];"
    "requestAnimationFrame(function () { rfRan.push(1); });"
    "requestAnimationFrame(function () { rfRan.push(2); });"
    "cancelAnimationFrame(location.hash.length);"
    "requestAnimationFrame(function () { fetch('/api/rafcancel?ran=' + rfRan.join('-')); });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
