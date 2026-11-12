/* THE MIXED-ROOT DOCUMENT: branches whose subject's root holds ECMAScript §21.3.2.28 Math.random ( ), an environment
   unknown, beside an attacker source (window.name) and beside absent application state (the cookies the jar lacks).
   Each condition is false on its example, so the request behind it is sent only on the arm against the example, and
   its record states that arm's price (`assumes`): one `environment-arm` whatever shares the root. A branch over
   window.name alone is the control: an attacker source's arms cost nothing. One of test_forced.c's exploring
   documents (TF_DOCS); it ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_ENVMIX_H
#define ENGINE_HOST_FIXTURE_ENVMIX_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_ENVMIX[] =
    "<!doctype html><html><body>"
    "<script>"
    "if (Math.random() + window.name.length > 3) fetch('/api/mxatk');"
    "if (Math.random() + document.cookie.length > 4096) fetch('/api/mxck');"
    "if (window.name.length > 3) fetch('/api/mxfree');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
