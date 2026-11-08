/* THE SVG LOCAL NAME DOCUMENT (`svglocalname`): HTML §13.2.6.5 The rules for parsing tokens in foreign content
   changes an SVG start tag's name to its adjusted spelling, so a parsed element's local name is that spelling
   and is the same name createElementNS gives. The page reads, over markup assigned to innerHTML and over the
   parse of the document itself: the localName of parsed `linearGradient`, `foreignObject`, `clipPath` and
   `feGaussianBlur`; that the lowercase end tag `</lineargradient>` closes the element (the rect after it is the
   svg's child); that a type selector matches in the element's own case only (HTML §4.16.2 Case-sensitivity of
   selectors); that a parsed `foreignObject` is an HTML integration point (its `div` is in the HTML namespace);
   and that a parsed element and a createElementNS one of the same name answer the same selector. Chrome gives
   the same answers.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SVGLOCALNAME_H
#define ENGINE_HOST_FIXTURE_SVGLOCALNAME_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SVGLOCALNAME[] =
    "<!doctype html><html><body>"
    "<svg id=lnDoc><clipPath id=lnClip></clipPath><feGaussianBlur id=lnBlur></feGaussianBlur></svg>"
    "<div id=lnHost></div>"
    "<script>"
    "var lnH = document.getElementById('lnHost');"
    "lnH.innerHTML = '<svg><linearGradient id=lnG><stop></stop></lineargradient><rect id=lnR></rect>'"
    " + '<foreignObject id=lnF><div id=lnD>x</div></foreignObject></svg>';"
    "var lnNames = [document.getElementById('lnG'), document.getElementById('lnF'),"
    " document.getElementById('lnClip'), document.getElementById('lnBlur')]"
    ".map(function (e) { return e.localName; }).join('.');"
    "var lnEt = document.getElementById('lnR').parentNode.localName;"
    "var lnQs = (lnH.querySelector('linearGradient') ? 1 : 0) + '' + (lnH.querySelector('lineargradient') ? 1 : 0);"
    "var lnFo = document.getElementById('lnD').namespaceURI === 'http://www.w3.org/1999/xhtml' ? 1 : 0;"
    "var lnMade = document.createElementNS('http://www.w3.org/2000/svg', 'linearGradient');"
    "lnH.firstChild.appendChild(lnMade);"
    "var lnSame = lnH.querySelectorAll('linearGradient').length;"
    "fetch('/api/svglocalname?names=' + lnNames + '&et=' + lnEt + '&qs=' + lnQs + '&fo=' + lnFo"
    " + '&same=' + lnSame);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
