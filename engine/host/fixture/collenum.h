/* THE COLLECTION-ENUMERATION DOCUMENTS: the internal methods of DOM §4.2.10's live collections that the page reaches
   through a builtin rather than a property read, one document each so that one failing does not hide the others.
   `collforin` runs a for-in over `children` (Web IDL §3.9.6 [[OwnPropertyKeys]], then each key's
   [[GetOwnProperty]]); `collkeys` reads Object.keys and Object.getOwnPropertyNames of it; `collhasown` asks
   hasOwnProperty of a fresh collection, whose index cache holds nothing; `collexpando` writes and reads an
   expando on an HTMLCollection (Web IDL §3.9.2 [[Set]]).
   test_forced.c's exploring documents (its TF_DOCS table). Each ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COLLENUM_H
#define ENGINE_HOST_FIXTURE_COLLENUM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define COLLENUM_BOX "<div id=ceBox><b id=ce0></b><b id=ce1></b><b id=ce2></b></div>"

static const char HTML_COLLFORIN[] =
    "<!doctype html><html><body>" COLLENUM_BOX
    "<script>"
    "var ceK = [];"
    "for (var k in document.getElementById('ceBox').children) if (/^[0-9]+$/.test(k)) ceK.push(k);"
    "fetch('/api/collforin?k=' + ceK.join('-'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_COLLKEYS[] =
    "<!doctype html><html><body>" COLLENUM_BOX
    "<script>"
    "var ceC = document.getElementById('ceBox').children;"
    "fetch('/api/collkeys?k=' + Object.keys(ceC).join('-') + '&n=' + Object.getOwnPropertyNames(ceC).join('-'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_COLLHASOWN[] =
    "<!doctype html><html><body>" COLLENUM_BOX
    "<script>"
    "var ceT = document.getElementById('ceBox').getElementsByTagName('b');"
    "fetch('/api/collhasown?h=' + (ceT.hasOwnProperty(1) ? 1 : 0) + (ceT.hasOwnProperty(5) ? 1 : 0));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_COLLEXPANDO[] =
    "<!doctype html><html><body>" COLLENUM_BOX
    "<script>"
    "var ceE = document.getElementById('ceBox').getElementsByTagName('b');"
    "ceE.ceFoo = 5;"
    "fetch('/api/collexpando?e=' + ceE.ceFoo + '&l=' + ceE.length);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
