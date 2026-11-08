/* THE REMOVE-REST DOCUMENT: a `<div>` holding 100,000 empty `<i>` elements (test_forced.c's COLLREST_ITEMS,
   twice), which the page's one classic program removes with removeChild. DOM §4.2.3's remove walks the node's
   shadow-including descendants for the removing steps, and the name index's upkeep walks them too, each charged
   as one DOM-walk unit, so one removal is more work than a quantum: a removal that rests per node ends a slice
   inside the call, and the scheduler's widest stretch between two offers on that arm stays under a quantum.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_REMOVEREST_H
#define ENGINE_HOST_FIXTURE_REMOVEREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REMOVEREST[] =
    "<!doctype html><html><body>"
    "<div id=rrBox>" COLLREST_ITEMS COLLREST_ITEMS "</div>"
    "<script>"
    "var rrBox = document.getElementById('rrBox');"
    "document.body.removeChild(rrBox);"
    "fetch('/api/rr-done?c=' + rrBox.childNodes.length);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
