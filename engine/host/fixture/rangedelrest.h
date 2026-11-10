/* THE RANGE-DELETE-REST DOCUMENT: a `<div>` box holding a named `<img>` (display:none), an `<i>` with an id and
   100,000 empty `<i>` (test_forced.c's COLLREST_ITEMS, twice). The page forks on `location.hash`: the example's
   arm selects the box with a Range and deletes its contents, whose DOM §5.5 deleteContents step 10 removes the
   box with remove's walks of its 100,002 children, more work than a quantum, so the call rests inside itself;
   the other arm, which the scheduler can run while the first is parked mid-removal, reads the box in place and
   both names present. Each arm reports what it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RANGEDELREST_H
#define ENGINE_HOST_FIXTURE_RANGEDELREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RANGEDELREST[] =
    "<!doctype html><html><body>"
    "<div id=rdBox><img name=rdNamed style=display:none><i id=rdInner></i>" COLLREST_ITEMS COLLREST_ITEMS "</div>"
    "<script>"
    "var rdBox = document.getElementById('rdBox');"
    "if (location.hash === '#keep') {"
    "  fetch('/api/rd-kept?p=' + (rdBox.parentNode === document.body ? 1 : 0) + '&c=' + rdBox.childNodes.length +"
    "        '&n=' + (document.rdNamed ? 1 : 0) + '&w=' + (window.rdInner ? 1 : 0));"
    "} else {"
    "  var rdR = document.createRange();"
    "  rdR.selectNode(rdBox);"
    "  rdR.deleteContents();"
    "  fetch('/api/rd-done?p=' + (rdBox.parentNode === null ? 1 : 0) + '&c=' + rdBox.childNodes.length +"
    "        '&n=' + (document.rdNamed === undefined ? 1 : 0) + '&w=' + (window.rdInner === undefined ? 1 : 0) +"
    "        '&k=' + (rdR.collapsed ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
