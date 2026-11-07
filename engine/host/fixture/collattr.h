/* THE COLLECTION-ATTRIBUTE DOCUMENT: live collections whose membership is decided by an attribute, read before
   and after a write to that attribute on one of their candidates. DOM §4.2.10's collections are live, so each
   later read answers the tree as it is then: getElementsByClassName after `classList.add`, getElementsByName
   after `setAttribute('name')`, `document.links` after `setAttribute('href')`, and a named key after `id` is set.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_COLLATTR_H
#define ENGINE_HOST_FIXTURE_COLLATTR_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_COLLATTR[] =
    "<!doctype html><html><body>"
    "<div id=caBox><p id=caP></p><input id=caIn><a id=caA>x</a></div>"
    "<script>"
    "var caP = document.getElementById('caP'), caIn = document.getElementById('caIn');"
    "var caA = document.getElementById('caA'), caBox = document.getElementById('caBox');"
    "var caCls = document.getElementsByClassName('caX'), caNm = document.getElementsByName('caN');"
    "var caLk = document.links, caCh = caBox.getElementsByTagName('*');"
    "var c0 = caCls.length, n0 = caNm.length, l0 = caLk.length, k0 = caCh.caK ? 1 : 0;"
    "caP.classList.add('caX'); caIn.setAttribute('name', 'caN'); caA.setAttribute('href', '#x'); caP.id = 'caK';"
    "var c1 = caCls.length, n1 = caNm.length, l1 = caLk.length, k1 = caCh.caK === caP ? 1 : 0;"
    "caP.classList.remove('caX');"
    "var c2 = caCls.length;"
    "fetch('/api/collattr?c=' + c0 + c1 + c2 + '&n=' + n0 + n1 + '&l=' + l0 + l1 + '&k=' + k0 + k1);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
