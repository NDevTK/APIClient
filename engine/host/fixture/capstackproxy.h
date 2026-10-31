/* THE CAPTURE-STACK-PROXY DOCUMENT: Error.captureStackTrace (proposal-error-capturestacktrace §20.5.2.1) on a
   Proxy whose `defineProperty` trap is the page's own code. The data-property strategy's DefinePropertyOrThrow
   reaches the trap, so it runs as part of the call and forwards the descriptor. The witness carries how many
   times the trap's loop ran, the installed value's type and whether it is enumerable.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CAPSTACKPROXY_H
#define ENGINE_HOST_FIXTURE_CAPSTACKPROXY_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CAPSTACKPROXY[] =
    "<!doctype html><html><body>"
    "<script>"
    "var csN = 0;"
    "var csP = new Proxy({}, { defineProperty: function (t, k, d) {"
    "  for (var i = 0; i < 3; i++) csN++; return Reflect.defineProperty(t, k, d); } });"
    "Error.captureStackTrace(csP);"
    "var csD = Object.getOwnPropertyDescriptor(csP, 'stack');"
    "fetch('/api/csp-define?got=' + csN + '-' + typeof csD.value + '-' + csD.enumerable);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
