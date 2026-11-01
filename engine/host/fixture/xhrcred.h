/* THE XHR-CREDENTIALS DOCUMENT: XHR §3.5.1 open() with a username and password read from a fetched JSON reply.
   Step 8 sets them into the parsed URL, so the request's address is a derivation over the address and both
   credentials rather than bytes coerced from an unknown. The page reports that open() returned, then sends.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_XHRCRED_H
#define ENGINE_HOST_FIXTURE_XHRCRED_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_XHRCRED[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var cfg = await (await fetch('/api/xhrcred-cfg')).json();"
    "  var x = new XMLHttpRequest();"
    "  x.open('GET', '/api/xhrcred-data', true, cfg.user, cfg.pass);"
    "  fetch('/api/xhrcred-opened');"
    "  x.send();"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
