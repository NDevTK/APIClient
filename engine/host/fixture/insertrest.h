/* THE INSERT-REST DOCUMENT: a `<div>` holding a named `<img>` (display:none), an `<i>` with an id and 100,000
   empty `<i>` (test_forced.c's COLLREST_ITEMS, twice), and an empty target `<div>`. The page's one classic
   program removes the box, then forks on `location.hash`: the example's arm appends the detached box to the
   target with appendChild, whose walks of the subtree (the name index's upkeep, DOM §4.2.3 insert step 7.7's
   insertion steps, step 11's staticNodeList) are more work than a quantum, so the call rests inside itself; the
   other arm, which the scheduler can run while the first is parked mid-insert, reads the box still detached and
   both names absent. Each arm reports what it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_INSERTREST_H
#define ENGINE_HOST_FIXTURE_INSERTREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_INSERTREST[] =
    "<!doctype html><html><body>"
    "<div id=irBox><img name=irNamed style=display:none><i id=irInner></i>" COLLREST_ITEMS COLLREST_ITEMS "</div>"
    "<div id=irTarget></div>"
    "<script>"
    "var irBox = document.getElementById('irBox'), irT = document.getElementById('irTarget');"
    "document.body.removeChild(irBox);"
    "if (location.hash === '#keep') {"
    "  fetch('/api/ir-kept?p=' + (irBox.parentNode === null ? 1 : 0) + '&c=' + irBox.childNodes.length +"
    "        '&n=' + (document.irNamed === undefined ? 1 : 0) + '&w=' + (window.irInner === undefined ? 1 : 0));"
    "} else {"
    "  irT.appendChild(irBox);"
    "  fetch('/api/ir-done?p=' + (irBox.parentNode === irT ? 1 : 0) + '&c=' + irBox.childNodes.length +"
    "        '&n=' + (document.irNamed ? 1 : 0) + '&w=' + (window.irInner ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
