/* The header-value-class document: requests whose header values are bytes the page wrote, an unknown with an
   example (`navigator.language`), and an unknown with none (`location.hash`), set through a fetch() init
   record, through a Headers object's set and append, and through XMLHttpRequest's setRequestHeader, where two
   values of one name combine. A host is shown each value as the bytes it would send with the class of those
   bytes: the example for an unknown that has one, the display shape for one that has none.
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
    "fetch('/api/hc-unk', { headers: { 'X-Frag': location.hash } });"
    "</script>"
    "<script>"
    "var hcH = new Headers();"
    "hcH.set('X-Lang', navigator.language);"
    "hcH.append('X-Frag', location.hash);"
    "fetch('/api/hc-members', { headers: hcH });"
    "</script>"
    "<script>"
    "var hcX = new XMLHttpRequest();"
    "hcX.open('GET', '/api/hc-xhr');"
    "hcX.setRequestHeader('X-Lang', navigator.language);"
    "hcX.setRequestHeader('X-Both', 'a');"
    "hcX.setRequestHeader('X-Both', navigator.language);"
    "hcX.setRequestHeader('X-Frag', 'b');"
    "hcX.setRequestHeader('X-Frag', location.hash);"
    "hcX.send();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
