/* THE LINK-REST DOCUMENT: a `<link rel=stylesheet>` whose sheet (test_forced.c's LINKREST_SHEET, served as
   text/css) is 10,001 rules and about 180 KiB, so HTML §4.6.8.23's process steps parse and build more than a
   work quantum of it. The page finds the sheet in `document.styleSheets` by its owner node and reads it back
   through CSSOM and the cascade once `load` has fired, or at once if the sheet is already there: a build that
   rested between quanta answers the sheet one that did not does.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_LINKREST_H
#define ENGINE_HOST_FIXTURE_LINKREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_LINKREST[] =
    "<!doctype html><html><head><link id=lrLink rel=stylesheet href='/sheet/linkrest.css'></head><body>"
    "<div id=lrEl></div>"
    "<script>"
    "var lrL = document.getElementById('lrLink');"
    "function lrSheet() {"
    "  for (var i = 0; i < document.styleSheets.length; i++)"
    "    if (document.styleSheets[i].ownerNode === lrL) return document.styleSheets[i];"
    "  return null;"
    "}"
    "function lrReport() {"
    "  var r = lrSheet().cssRules;"
    "  fetch('/api/linkrest?n=' + r.length + '&f=' + encodeURIComponent(r[0].selectorText) +"
    "        '&w=' + r[r.length - 1].style.width + '&c=' + getComputedStyle(document.getElementById('lrEl')).width);"
    "}"
    "if (lrSheet()) lrReport(); else lrL.addEventListener('load', lrReport);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
