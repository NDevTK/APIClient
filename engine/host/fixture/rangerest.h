/* THE RANGE-REST DOCUMENT: a `<div>` box holding a named `<img>` (display:none), an `<i>` with an id and 100,000
   empty `<i>` (test_forced.c's COLLREST_ITEMS, twice). The page forks on `location.hash`: the example's arm
   selects the box with a Range and extracts it, whose DOM §5.5 extract step 18 appends the box to the fragment,
   and that pre-insert's adopt removes it from the document with remove's walks of its 100,002 children, more
   work than a quantum, so the call rests inside itself; the other arm, which the scheduler can run while the
   first is parked mid-extract, reads the box in place and both names present. Each arm reports what it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_RANGEREST_H
#define ENGINE_HOST_FIXTURE_RANGEREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_RANGEREST[] =
    "<!doctype html><html><body>"
    "<div id=rgBox><img name=rgNamed style=display:none><i id=rgInner></i>" COLLREST_ITEMS COLLREST_ITEMS "</div>"
    "<script>"
    "var rgBox = document.getElementById('rgBox');"
    "if (location.hash === '#keep') {"
    "  fetch('/api/rg-kept?p=' + (rgBox.parentNode === document.body ? 1 : 0) + '&c=' + rgBox.childNodes.length +"
    "        '&n=' + (document.rgNamed ? 1 : 0) + '&w=' + (window.rgInner ? 1 : 0));"
    "} else {"
    "  var rgR = document.createRange();"
    "  rgR.selectNode(rgBox);"
    "  var rgF = rgR.extractContents();"
    "  fetch('/api/rg-done?p=' + (rgBox.parentNode === rgF ? 1 : 0) + '&c=' + rgBox.childNodes.length +"
    "        '&n=' + (document.rgNamed === undefined ? 1 : 0) + '&w=' + (window.rgInner === undefined ? 1 : 0) +"
    "        '&k=' + (rgR.collapsed ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
