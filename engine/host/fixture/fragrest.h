/* THE FRAGMENT-REST DOCUMENT: fixture/insertrest.h's box, moved into a DocumentFragment and appended from there.
   A `<div>` holding a named `<img>` (display:none), an `<i>` with an id and 100,000 empty `<i>` (test_forced.c's
   COLLREST_ITEMS, twice), and an empty target `<div>`. The page's one classic program removes the box, appends it
   to a fragment, then forks on `location.hash`: the example's arm appends the fragment to the target, whose DOM
   §4.2.3 insert takes the fragment's children and walks each one's subtree, more work than a quantum, so the call
   rests inside itself; the other arm, which the scheduler can run while the first is parked mid-insert, reads the
   box still in the fragment and both names absent. Each arm reports what it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRAGREST_H
#define ENGINE_HOST_FIXTURE_FRAGREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FRAGREST[] =
    "<!doctype html><html><body>"
    "<div id=frBox><img name=frNamed style=display:none><i id=frInner></i>" COLLREST_ITEMS COLLREST_ITEMS "</div>"
    "<div id=frTarget></div>"
    "<script>"
    "var frBox = document.getElementById('frBox'), frT = document.getElementById('frTarget');"
    "var frF = document.createDocumentFragment();"
    "document.body.removeChild(frBox);"
    "frF.appendChild(frBox);"
    "if (location.hash === '#keep') {"
    "  fetch('/api/fr-kept?p=' + (frBox.parentNode === frF ? 1 : 0) + '&c=' + frBox.childNodes.length +"
    "        '&n=' + (document.frNamed === undefined ? 1 : 0) + '&w=' + (window.frInner === undefined ? 1 : 0));"
    "} else {"
    "  frT.appendChild(frF);"
    "  fetch('/api/fr-done?p=' + (frBox.parentNode === frT ? 1 : 0) + '&c=' + frBox.childNodes.length +"
    "        '&f=' + frF.childNodes.length + '&n=' + (document.frNamed ? 1 : 0) + '&w=' + (window.frInner ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
