/* THE ADOPT-REST DOCUMENT: fixture/insertrest.h's box, moved into another document and back. A `<div>` holding a
   named `<img>` (display:none), an `<i>` with an id and 100,000 empty `<i>` (test_forced.c's COLLREST_ITEMS,
   twice), and an empty target `<div>`. The page's one classic program removes the box and creates a second
   document, then forks on `location.hash`: '#keep' reads the box detached and still this document's; '#an'
   adopts it into the second document with adoptNode; the example's arm appends it to the second document's body
   and then to the target. Each move runs DOM §4.5 adopt's step 3 walk over the box, more work than a quantum, so
   each call rests inside itself. Each arm reports what it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ADOPTREST_H
#define ENGINE_HOST_FIXTURE_ADOPTREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ADOPTREST[] =
    "<!doctype html><html><body>"
    "<div id=adBox><img name=adNamed style=display:none><i id=adInner></i>" COLLREST_ITEMS COLLREST_ITEMS "</div>"
    "<div id=adTarget></div>"
    "<script>"
    "var adBox = document.getElementById('adBox'), adT = document.getElementById('adTarget');"
    "var adI = document.getElementById('adInner'), adH = location.hash;"
    "document.body.removeChild(adBox);"
    "var adD = document.implementation.createHTMLDocument('');"
    "if (adH === '#keep') {"
    "  fetch('/api/ad-kept?o=' + (adI.ownerDocument === document ? 1 : 0) + '&c=' + adBox.childNodes.length);"
    "} else if (adH === '#an') {"
    "  adD.adoptNode(adBox);"
    "  fetch('/api/ad-an?o=' + (adI.ownerDocument === adD ? 1 : 0) + '&c=' + adBox.childNodes.length);"
    "} else {"
    "  adD.body.appendChild(adBox);"
    "  var adMid = adI.ownerDocument === adD ? 1 : 0;"
    "  adT.appendChild(adBox);"
    "  fetch('/api/ad-done?m=' + adMid + '&o=' + (adI.ownerDocument === document ? 1 : 0) + '&p=' +"
    "        (adBox.parentNode === adT ? 1 : 0) + '&c=' + adBox.childNodes.length +"
    "        '&n=' + (document.adNamed ? 1 : 0) + '&w=' + (window.adInner ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
