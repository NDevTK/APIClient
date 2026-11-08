/* THE SLOT-REST DOCUMENT: a shadow host whose declarative shadow root holds a `<p>` with the only `<slot>`, which
   the host's `<span slot=a>` is assigned to, and a `<div>` of 100,000 empty `<i>` (test_forced.c's
   COLLREST_ITEMS, twice). The page's one classic program removes the `<p>` from the shadow root with removeChild:
   the `<p>` has an inclusive descendant that is a slot, so DOM §4.2.3 remove step 10.1 runs assign slottables
   for a tree over the shadow root, a walk of its 100,000 remaining nodes, and step 10.2 empties the removed slot.
   A removal that rests per node ends a slice inside that walk. The page reports the span's assignment before and
   after, and the div's children. The host is display:none: assignment is the DOM's, and a laid-out shadow tree's
   block child aborts the painter's containing-block walk at the shadow root (core/layout/used_value.c).
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c
   defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SLOTREST_H
#define ENGINE_HOST_FIXTURE_SLOTREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SLOTREST[] =
    "<!doctype html><html><body>"
    "<div id=srHost style=display:none><template shadowrootmode=open><p><slot name=a></slot></p>"
    "<div>" COLLREST_ITEMS COLLREST_ITEMS "</div></template><span id=srSpan slot=a>x</span></div>"
    "<script>"
    "var srRoot = document.getElementById('srHost').shadowRoot;"
    "var srSpan = document.getElementById('srSpan');"
    "var srWas = srSpan.assignedSlot !== null ? 1 : 0;"
    "srRoot.removeChild(srRoot.firstChild);"
    "fetch('/api/sr-done?w=' + srWas + '&a=' + (srSpan.assignedSlot === null ? 1 : 0) +"
    "      '&c=' + srRoot.lastChild.childNodes.length);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
