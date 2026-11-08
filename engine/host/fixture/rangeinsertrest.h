/* THE RANGE-INSERT-REST DOCUMENT: a `<div>` box holding a named `<img>` (display:none), an `<i>` with an id and
   100,000 empty `<i>` (test_forced.c's COLLREST_ITEMS, twice), and an empty target `<p>` holding one Text node.
   The page forks on `location.hash`: the example's arm collapses a Range inside the Text node and inserts the box
   there, whose DOM §5.5 "insert a node into a live range" splits the Text node (step 7), removes the box from the
   document (step 9) and pre-inserts it into the target (step 12), each with the walks of its 100,002 children,
   more work than a quantum, so the call rests inside itself; the other arm, which the scheduler can run while
   the first is parked mid-insert, reads the box in place and the target's one Text node. Each arm reports what
   it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RANGEINSERTREST_H
#define ENGINE_HOST_FIXTURE_RANGEINSERTREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RANGEINSERTREST[] =
    "<!doctype html><html><body>"
    "<div id=riBox><img name=riNamed style=display:none><i id=riInner></i>" COLLREST_ITEMS COLLREST_ITEMS "</div>"
    "<p id=riTarget>abcd</p>"
    "<script>"
    "var riBox = document.getElementById('riBox'), riT = document.getElementById('riTarget');"
    "if (location.hash === '#keep') {"
    "  fetch('/api/ri-kept?p=' + (riBox.parentNode === document.body ? 1 : 0) + '&c=' + riBox.childNodes.length +"
    "        '&t=' + riT.childNodes.length + '&n=' + (document.riNamed ? 1 : 0) + '&w=' + (window.riInner ? 1 : 0));"
    "} else {"
    "  var riR = document.createRange();"
    "  riR.setStart(riT.firstChild, 2);"
    "  riR.insertNode(riBox);"
    "  fetch('/api/ri-done?p=' + (riBox.parentNode === riT ? 1 : 0) + '&c=' + riBox.childNodes.length +"
    "        '&s=' + riT.firstChild.data + '-' + riT.lastChild.data + '&l=' + riT.childNodes.length +"
    "        '&e=' + (riR.endContainer === riT && riR.endOffset === 2 ? 1 : 0) +"
    "        '&n=' + (document.riNamed ? 1 : 0) + '&w=' + (window.riInner ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
