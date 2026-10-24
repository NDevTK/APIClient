/* THE WEAK-KEY CHAIN DOCUMENT: 300000 symbols, each a WeakMap key whose value is the next, so the only strong
   reference to symbol i+1 is the entry keyed by symbol i. When the function holding the first returns, its
   release resets the first entry, which releases the second symbol, whose release resets the second entry,
   and so on to the end of the chain: the removals ECMAScript §9.9.3 Execution permits once §9.9.2 Liveness
   finds a key not live, as one cascade of releases as long as the chain. The fetch after the return is
   reached only if that cascade completes.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_WEAKCHAIN_H
#define ENGINE_HOST_FIXTURE_WEAKCHAIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_WEAKCHAIN[] =
    "<!doctype html><html><body>"
    "<script>"
    "var wcMap = new WeakMap();"
    "function wcBuild(n) {"
    "  var head = Symbol('h'), k = head;"
    "  for (var i = 0; i < n; i++) { var s = Symbol(); wcMap.set(k, s); k = s; }"
    "  return n;"
    "}"
    "var wcN = wcBuild(300000);"
    "fetch('/api/weakchain?d=' + wcN);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
