/* THE CHILD-REMOVE-REST DOCUMENT: fixture/removerest.h's document with the box removed by DOM §4.2.8 ChildNode's
   remove() instead of removeChild. A `<div>` holding a named `<img>` (display:none), an `<i>` with an id and
   100,000 empty `<i>` (test_forced.c's COLLREST_ITEMS, twice); the page forks on `location.hash`. The example's
   arm's removal walks the subtree (the name index's upkeep, DOM §4.2.3 remove steps 10 and 14), more work than a
   quantum, so the call rests inside itself; the other arm, which the scheduler can run while the first is parked
   mid-removal, reads the div in place and both names. Each arm reports what it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CHILDREMOVEREST_H
#define ENGINE_HOST_FIXTURE_CHILDREMOVEREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CHILDREMOVEREST[] =
    "<!doctype html><html><body>"
    "<div id=crBox><img name=crNamed style=display:none><i id=crInner></i>" COLLREST_ITEMS COLLREST_ITEMS "</div>"
    "<script>"
    "var crBox = document.getElementById('crBox');"
    "if (location.hash === '#keep') {"
    "  fetch('/api/cr-kept?p=' + (crBox.parentNode === document.body ? 1 : 0) + '&c=' + crBox.childNodes.length +"
    "        '&n=' + (document.crNamed ? 1 : 0) + '&w=' + (window.crInner ? 1 : 0));"
    "} else {"
    "  crBox.remove();"
    "  fetch('/api/cr-done?c=' + crBox.childNodes.length + '&n=' + (document.crNamed === undefined ? 1 : 0) +"
    "        '&w=' + (window.crInner === undefined ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
