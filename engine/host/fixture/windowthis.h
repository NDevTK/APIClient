/* THE WINDOW IDENTITY DOCUMENT: HTML §7.2.2's `window`, `self` and `frames`, ECMAScript §19.1.1's `globalThis`,
   the top-level `this` (§16.1.6 ScriptEvaluation) and a sloppy function's undefined receiver (§10.2.1.2 step
   5.a) are all the realm's [[GlobalThisValue]], the navigable's WindowProxy, which is also HTML §7.2.2's
   `document.defaultView`, a top-level navigable's `parent` and `top`, and a Window listener's `this` and
   `event.currentTarget`. One of test_forced.c's exploring documents (its TF_DOCS table). Each comparison is
   reported as 1 or 0. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_WINDOWTHIS_H
#define ENGINE_HOST_FIXTURE_WINDOWTHIS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_WINDOWTHIS[] =
    "<!doctype html><html>"
    "<body>"
    "<script>var top_this = this;"
    "function sloppy() { return this; }"
    "function b(x) { return x ? 1 : 0; }"
    "var lt = 0, lc = 0;"
    "addEventListener('wt', function (e) { lt = b(this === window); lc = b(e.currentTarget === window); });"
    "dispatchEvent(new Event('wt'));"
    "fetch('/api/winthis?dv=' + b(document.defaultView === window)"
    " + '&th=' + b(top_this === window)"
    " + '&gt=' + b(globalThis === window)"
    " + '&se=' + b(self === window)"
    " + '&fr=' + b(frames === window)"
    " + '&ww=' + b(window.window === window)"
    " + '&sl=' + b(sloppy() === window)"
    " + '&pa=' + b(window.parent === window)"
    " + '&tp=' + b(top === window)"
    " + '&lt=' + lt + '&lc=' + lc"
    " + '&fn=' + typeof window.addEventListener);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
