/* THE TEXT-REST DOCUMENT: a `<div>` whose children are a named `<img>` (display:none), an `<i>` with an id and
   100,000 empty `<i>` (test_forced.c's COLLREST_ITEMS, twice). The page forks on `location.hash`: the example's
   arm sets the div's textContent, whose DOM §4.2.3 replace all removes 100,002 children, each with its walks
   (the name index's upkeep, remove steps 10 and 14), and inserts one Text node, more work than a quantum, so the
   setter rests inside itself; the other arm, which the scheduler can run while the first is parked mid-replace,
   reads the children in place and both names present. Each arm reports what it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_TEXTREST_H
#define ENGINE_HOST_FIXTURE_TEXTREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_TEXTREST[] =
    "<!doctype html><html><body>"
    "<div id=trBox><img name=trNamed style=display:none><i id=trInner></i>" COLLREST_ITEMS COLLREST_ITEMS "</div>"
    "<script>"
    "var trBox = document.getElementById('trBox');"
    "if (location.hash === '#keep') {"
    "  fetch('/api/tr-kept?c=' + trBox.childNodes.length + '&n=' + (document.trNamed ? 1 : 0) +"
    "        '&w=' + (window.trInner ? 1 : 0));"
    "} else {"
    "  trBox.textContent = 'done';"
    "  fetch('/api/tr-done?c=' + trBox.childNodes.length + '&t=' + (trBox.firstChild.data === 'done' ? 1 : 0) +"
    "        '&n=' + (document.trNamed === undefined ? 1 : 0) + '&w=' + (window.trInner === undefined ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
