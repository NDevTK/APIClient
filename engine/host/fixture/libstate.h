/* THE PAGE-STATE REQUEST DOCUMENT: control-libs' `/api/lib-state` shape without the library. An inline script
   publishes `lsState`, and the request body is `JSON.stringify` over its members, behind the
   `typeof data === 'undefined'` test axios's request path makes of the transformed body. The served member
   is minted unknown with its value as the example (solver/absent.c's present arm), so the world this document
   is served in sends the request without assuming anything of `lsState`. Two more requests sit behind tests
   jQuery's boot makes on every page: its expando (`version + Math.random()` with the non-digits removed)
   compared with a function, and a user-agent regular expression; both test environment unknowns, not
   application state, so neither is an assumption.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_LIBSTATE_H
#define ENGINE_HOST_FIXTURE_LIBSTATE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_LIBSTATE[] =
    "<!doctype html><html><body>"
    "<script>"
    "var lsState = { user: 'u-1042', cart: ['sku-7', 'sku-9'], currency: 'EUR' };"
    "function lsTransform(data) { return JSON.stringify(data); }"
    "var lsBody = lsTransform({ user: lsState.user, items: lsState.cart.length, currency: lsState.currency });"
    "if (typeof lsBody === 'undefined') lsBody = null;"
    "fetch('/api/ls-state', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: lsBody });"
    "var lsExpando = ('3.7.1' + Math.random()).replace(/\\D/g, '');"
    "function lsFn() {}"
    "if (lsFn !== lsExpando) fetch('/api/ls-random');"
    "var lsOldIE = /Trident\\/|MSIE /.test(navigator.userAgent);"
    "if (!lsOldIE) fetch('/api/ls-ua');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
