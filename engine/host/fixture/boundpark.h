/* THE BOUND-CALLBACK DOCUMENT: one bound function body handed to four of the routes real pages call a callback
   through — a timer, an event listener reached by dispatchEvent, a promise reaction and an array callback. The
   body sends a synchronous XMLHttpRequest the host declines (a POST to /api/decl), so the waiting arm parks
   inside the bound callback and the failure arm takes the network error and reports which route it came from
   and which `this` it ran with (ECMAScript §10.4.1.1 [[Call]] ( thisArg, argList ): the bound one). None of the
   routes may reach the bound function's C class hook, which refuses the call.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_BOUNDPARK_H
#define ENGINE_HOST_FIXTURE_BOUNDPARK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_BOUNDPARK[] =
    "<!doctype html><html><body>"
    "<script>"
    "function bpBody(path) {"
    "  var x = new XMLHttpRequest(), e = 'none';"
    "  x.open('POST', '/api/decl/bp', false);"
    "  try { x.send('b'); } catch (err) { e = err.name; }"
    "  fetch(path + '?e=' + e + '&t=' + this.tag);"
    "}"
    "var bpThis = { tag: 'bound' };"
    "setTimeout(bpBody.bind(bpThis, '/api/boundpark/timer'), 0);"
    "document.addEventListener('bp', bpBody.bind(bpThis, '/api/boundpark/event'));"
    "document.dispatchEvent(new Event('bp'));"
    "Promise.resolve().then(bpBody.bind(bpThis, '/api/boundpark/then'));"
    "[0].map(bpBody.bind(bpThis, '/api/boundpark/map'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
