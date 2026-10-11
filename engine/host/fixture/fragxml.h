/* THE XML FRAGMENT FORK DOCUMENT: a flow whose fork arrives while HTML §14.4's XML fragment parse is part-way
   through. The setter's target belongs to an XML document, so HTML §8.5.4's fragment parsing algorithm steps
   step 2 sends the markup to §14.4, whose machine takes one parse item per step; fifty thousand elements keep it
   parsing when the fixture's zone refuses `/api/cold/held.js` (test_forced.c's reply arm) and
   flow_decline_fork asks for a fork. Both arms finish the parse. One of test_forced.c's exploring documents (its
   TF_DOCS table). It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRAGXML_H
#define ENGINE_HOST_FIXTURE_FRAGXML_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FRAGXML[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    "var fxd = document.implementation.createDocument(null, 'r', null);"
    "fetch('/api/cold/held.js').catch(function () {});"
    "fxd.documentElement.innerHTML = '<a/>'.repeat(50000) + '<b>tail</b>';"
    "fetch('/api/fragxml?v=' + fxd.documentElement.childNodes.length"
    " + '&t=' + fxd.documentElement.lastChild.textContent);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
