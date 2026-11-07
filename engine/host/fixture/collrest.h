/* THE COLLECTION-REST DOCUMENT: a `<div>` holding 50,001 `<i>` elements (test_forced.c's COLLREST_ITEMS), so
   every walk of a live collection over it visits more nodes than a work quantum prices. The page reads DOM
   §4.2.10's members over three kinds: getElementsByTagName's descendant HTMLCollection (`length`, the last index,
   `item()`, `namedItem()` and the named key), the div's `children` (an index) and its `childNodes` (`length`). A
   walk that rested between quanta answers what one that did not does.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COLLREST_H
#define ENGINE_HOST_FIXTURE_COLLREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COLLREST[] =
    "<!doctype html><html><body>"
    "<div id=crBox>" COLLREST_ITEMS "<i id=crLast></i></div>"
    "<script>"
    "var crB = document.getElementById('crBox');"
    "var crA = document.getElementsByTagName('i');"
    "var crN = crA.length;"
    "var crL = crA[crN - 1];"
    "fetch('/api/collrest?n=' + crN + '&l=' + crL.id +"
    "      '&m=' + (crA.item(25000) === crB.children[25000] ? 1 : 0) +"
    "      '&k=' + (crA.namedItem('crLast') === crL ? 1 : 0) + (crA.crLast === crL ? 1 : 0) +"
    "      '&c=' + crB.childNodes.length);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
