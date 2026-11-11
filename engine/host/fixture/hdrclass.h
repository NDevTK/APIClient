/* The header-value-class document: requests whose header values are bytes the page wrote, an unknown with an
   example (`navigator.language`; `location.hash`, whose example is the document's own empty fragment), and an
   unknown with none (`navigator.language` on the arm that contradicts its example). They are written through a
   fetch() init record, through a Request's Headers object, through a Headers object given as an init (which
   Fetch §5.1's fill reads back through the object's iterator), and through XMLHttpRequest's setRequestHeader,
   where two values of one name combine. A Headers object's get() of an unknown value is compared with a
   literal, and its get() of two combined values is written to a request. A host is shown each value as the bytes it would send with the class
   of those bytes: the example for an unknown that has one, the display shape for one that has none.
   Witness addresses are constants. One of test_forced.c's exploring documents (its TF_DOCS table). It ends
   with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_HDRCLASS_H
#define ENGINE_HOST_FIXTURE_HDRCLASS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_HDRCLASS[] =
    "<!doctype html><html><body>"
    "<script>"
    "fetch('/api/hc-own', { headers: { 'X-Own': 'literal' } });"
    "fetch('/api/hc-ex', { headers: { 'X-Lang': navigator.language } });"
    "fetch('/api/hc-hash', { headers: { 'X-Frag': location.hash } });"
    "if (navigator.language !== 'en-US') fetch('/api/hc-unk', { headers: { 'X-Lang': navigator.language } });"
    "</script>"
    "<script>"
    "var hcR = new Request('/api/hc-members');"
    "hcR.headers.set('X-Lang', navigator.language);"
    "hcR.headers.append('X-Own', 'bytes');"
    "fetch(hcR);"
    "</script>"
    "<script>"
    "var hcH = new Headers();"
    "hcH.set('X-Lang', navigator.language);"
    "fetch('/api/hc-read', { headers: hcH });"
    "</script>"
    "<script>"
    "var hcG = new Headers({ 'X-Lang': navigator.language });"
    "if (hcG.get('x-lang') === 'fr-FR') fetch('/api/hc-get-fr'); else fetch('/api/hc-get-other');"
    "hcG.append('X-Lang', 'q');"
    "fetch('/api/hc-get-join', { headers: { 'X-Join': hcG.get('X-Lang') } });"
    "</script>"
    "<script>"
    "var hcX = new XMLHttpRequest();"
    "hcX.open('GET', '/api/hc-xhr');"
    "hcX.setRequestHeader('X-Lang', navigator.language);"
    "hcX.setRequestHeader('X-Both', 'a');"
    "hcX.setRequestHeader('X-Both', navigator.language);"
    "hcX.send();"
    "if (navigator.language !== 'en-US') {"
    "  var hcY = new XMLHttpRequest();"
    "  hcY.open('GET', '/api/hc-xhr-unk');"
    "  hcY.setRequestHeader('X-Both', 'a');"
    "  hcY.setRequestHeader('X-Both', navigator.language);"
    "  hcY.send();"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
