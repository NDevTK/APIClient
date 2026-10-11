/* THE FRAGMENT PLACEMENT FORK DOCUMENT: a flow whose fork arrives after HTML §13.4's feed has ended and while the
   innerHTML setter is still replacing its target's children. The target holds thirty thousand children, so the
   setter's FRAG_CLEAR runs one removal per step long after its eight-byte feed, and the fixture's zone refuses
   `/api/cold/held.js` (test_forced.c's reply arm), so flow_decline_fork asks for a fork at a pick inside that
   span. Both arms finish the operation. One of test_forced.c's exploring documents (its TF_DOCS table). It ends
   with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRAGPLACE_H
#define ENGINE_HOST_FIXTURE_FRAGPLACE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FRAGPLACE[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var fpd = document.createElement('div');"
    "fpd.innerHTML = '<i></i>'.repeat(30000);"
    "fetch('/api/cold/held.js').catch(function () {});"
    "fpd.innerHTML = '<b>x</b>';"
    "fetch('/api/fragplace?v=' + fpd.childNodes.length + '&t=' + fpd.textContent);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
