/* THE CONTINUATION-CHAIN DOCUMENT: a property read whose handler is a Proxy whose handler is a Proxy, 100000
   deep, so each GetMethod(handler, "get") of ECMAScript §10.5.8 [[Get]] ( propertyKey, receiver ) step 5 is
   itself a [[Get]] on the next handler and no JavaScript frame stands between the levels: the nesting is one
   chain of parked C-continuations. The innermost handler's trap sends a synchronous XMLHttpRequest the host declines (a POST
   to /api/decl), so the waiting arm parks inside that chain, and the session releases it with the frontier
   parked on refusals, which walks the whole chain (tramp_cont_free). The failure arm takes the network error
   through every level and reaches the fetch after it.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CONTCHAIN_H
#define ENGINE_HOST_FIXTURE_CONTCHAIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CONTCHAIN[] =
    "<!doctype html><html><body>"
    "<script>"
    "var ccDepth = 100000;"
    "var ccH = { get: function () {"
    "  var x = new XMLHttpRequest(); x.open('POST', '/api/decl/chain', false); x.send('c'); return 1; } };"
    "for (var ccI = 0; ccI < ccDepth; ccI++) ccH = new Proxy({}, ccH);"
    "var ccOut = 'none';"
    "try { new Proxy({}, ccH).x; ccOut = 'read'; } catch (e) { ccOut = e.name; }"
    "fetch('/api/contchain?d=' + ccDepth + '&out=' + ccOut);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
