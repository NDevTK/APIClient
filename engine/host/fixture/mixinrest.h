/* THE MIXIN-REST DOCUMENT: fixture/insertrest.h's box, inserted by DOM §4.2.6 ParentNode's append, §4.2.8
   ChildNode's replaceWith and §4.2.6 replaceChildren, each with a string beside it, so each converts its nodes
   into a DocumentFragment and inserts that. A `<div>` holding a named `<img>` (display:none), an `<i>` with an id
   and 100,000 empty `<i>` (test_forced.c's COLLREST_ITEMS, twice), and a target `<div>` holding a `<span>`. The
   page's one classic program removes the box, then forks on `location.hash`: '#keep' reads the box detached and
   both names absent; '#rw' replaces the span with the box and a string, '#rc' replaces the target's children with
   them, and the example's arm appends them. The three inserting arms' walks of the box are more work than a
   quantum, so each call rests inside itself. Each arm reports what it read.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MIXINREST_H
#define ENGINE_HOST_FIXTURE_MIXINREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MIXINREST[] =
    "<!doctype html><html><body>"
    "<div id=mxBox><img name=mxNamed style=display:none><i id=mxInner></i>" COLLREST_ITEMS COLLREST_ITEMS "</div>"
    "<div id=mxTarget><span id=mxSpan></span></div>"
    "<script>"
    "var mxBox = document.getElementById('mxBox'), mxT = document.getElementById('mxTarget');"
    "var mxS = document.getElementById('mxSpan'), mxH = location.hash, mxK = 'ap';"
    "document.body.removeChild(mxBox);"
    "if (mxH === '#keep') {"
    "  fetch('/api/mx-kept?p=' + (mxBox.parentNode === null ? 1 : 0) + '&c=' + mxBox.childNodes.length +"
    "        '&n=' + (document.mxNamed === undefined ? 1 : 0) + '&w=' + (window.mxInner === undefined ? 1 : 0));"
    "} else {"
    "  if (mxH === '#rw') { mxK = 'rw'; mxS.replaceWith(mxBox, 't'); }"
    "  else if (mxH === '#rc') { mxK = 'rc'; mxT.replaceChildren(mxBox, 't'); }"
    "  else mxT.append(mxBox, 't');"
    "  fetch('/api/mx-' + mxK + '?p=' + (mxBox.parentNode === mxT ? 1 : 0) + '&c=' + mxBox.childNodes.length +"
    "        '&s=' + (mxS.parentNode === mxT ? 1 : 0) + '&l=' + mxT.childNodes.length +"
    "        '&n=' + (document.mxNamed ? 1 : 0) + '&w=' + (window.mxInner ? 1 : 0));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
