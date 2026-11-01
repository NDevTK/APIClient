/* THE VALUELESS EVENT HANDLER DOCUMENT. A parsed `<div onclick>` holds the empty string (DOM §4.9.2 "Interface
   Attr"), which HTML §8.1.8.1 "Event handlers"' attribute change steps turn into a handler, so `onclick` is a
   function and getAttribute answers "". Its own document because the parsed tree's handler walk is where a
   valueless value used to abort, which would take every other row down with it. Chrome 153 (Playwright)
   answers t=function a=empty for this markup.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ATTRPRESHANDLER_H
#define ENGINE_HOST_FIXTURE_ATTRPRESHANDLER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ATTRPRESHANDLER[] =
    "<!doctype html><html><body><div id=oc onclick></div>"
    "<script>"
    "requestAnimationFrame(function () {"
    "  var d = document.getElementById('oc');"
    "  fetch('/api/onclickattr?t=' + typeof d.onclick + '&a=' + (d.getAttribute('onclick') === '' ? 'empty' : 'other'));"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
