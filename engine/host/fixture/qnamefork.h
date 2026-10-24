/* THE UNKNOWN QUALIFIED NAME DOCUMENT: createElementNS over a name that is unknown external input, the shape of an
   SVG factory driven with an unknown argument. DOM §1.4's validate-and-extract has three completions over such a
   name, so the page's try/catch reaches three fetches: the element, InvalidCharacterError and NamespaceError.
   createElement over the same name has §4.5 step 1's two: the element and InvalidCharacterError.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_QNAMEFORK_H
#define ENGINE_HOST_FIXTURE_QNAMEFORK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_QNAMEFORK[] =
    "<!doctype html><html><body>"
    "<script>"
    "var qnName = location.hash.slice(1);"
    "try {"
    "  var qnEl = document.createElementNS('http://www.w3.org/2000/svg', qnName);"
    "  fetch('/api/qname?arm=element');"
    "} catch (e) {"
    "  fetch('/api/qname?arm=' + e.name);"
    "}"
    "try {"
    "  var lnEl = document.createElement(qnName);"
    "  fetch('/api/lname?arm=element');"
    "} catch (e) {"
    "  fetch('/api/lname?arm=' + e.name);"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
