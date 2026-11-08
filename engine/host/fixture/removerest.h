/* THE REMOVE-REST DOCUMENT: a `<div>` holding a named `<img>` (display:none, so the line the empty `<i>` share
   holds no atomic inline), an `<i>` with an id and 100,000 empty `<i>` (test_forced.c's COLLREST_ITEMS, twice). The page forks on `location.hash`: the example's arm removes the div
   with removeChild, whose walks of the subtree (the name index's upkeep, DOM §4.2.3 remove steps 10 and 14) are
   more work than a quantum, so the call rests inside itself; the other arm, which the scheduler can run while the
   first is parked mid-removal, reads the div in place and both names through the document's and the window's
   named properties. Each arm reports what it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_REMOVEREST_H
#define ENGINE_HOST_FIXTURE_REMOVEREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REMOVEREST[] =
    "<!doctype html><html><body>"
    "<div id=rrBox><img name=rrNamed style=display:none><i id=rrInner></i>" COLLREST_ITEMS COLLREST_ITEMS "</div>"
    "<script>"
    "var rrBox = document.getElementById('rrBox');"
    "if (location.hash === '#keep') {"
    "  fetch('/api/rr-kept?p=' + (rrBox.parentNode === document.body ? 1 : 0) + '&c=' + rrBox.childNodes.length +"
    "        '&n=' + (document.rrNamed ? 1 : 0) + '&w=' + (window.rrInner ? 1 : 0));"
    "} else {"
    "  document.body.removeChild(rrBox);"
    "  fetch('/api/rr-done?c=' + rrBox.childNodes.length + '&n=' + (document.rrNamed === undefined ? 1 : 0) +"
    "        '&w=' + (window.rrInner === undefined ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
