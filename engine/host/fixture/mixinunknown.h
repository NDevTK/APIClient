/* THE MIXIN-UNKNOWN DOCUMENT: DOM §4.2.6's `(Node or DOMString)...` members over unknown external input. A URL
   parameter's found arm is a String in every world, so "convert nodes into a node" makes it a Text node, alone
   (`append`, steps 1-2) and beside a known string in a fragment (`replaceChildren`, steps 3-5); /api/mu-done
   reports the two children the second call leaves. One of test_forced.c's exploring documents (its TF_DOCS
   table). It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MIXINUNKNOWN_H
#define ENGINE_HOST_FIXTURE_MIXINUNKNOWN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MIXINUNKNOWN[] =
    "<!doctype html><html><body>"
    "<script>"
    "var muQ = new URLSearchParams(location.search).get('mu');"
    "var muD = document.createElement('div');"
    "if (muQ) {"
    "  muD.append(muQ);"
    "  muD.replaceChildren('a', muQ);"
    "  fetch('/api/mu-done?n=' + muD.childNodes.length);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
