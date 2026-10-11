/* THE DECLINES DOCUMENT: a page whose XMLHttpRequest and fetch() calls the zone refuses to send, so the session
   ends with its frontier exhausted and every survivor parked on a refusal. The host declines every request to
   /api/decl that is not a GET, and every one a fork or a drive composed (fixture_provide for a fetch(),
   hostreq_answer_all for an XMLHttpRequest's rendezvous), as the extension's unconfigured zone does. The
   XMLHttpRequest's send task parks on its rendezvous with the MessageChannel task queued behind it, so the
   waiting arm of that refusal rests holding a task its timeline has not reached. The GET to /api/decl/redir is
   served. The document is small, so the frontier exhausts before the host's first sample of its rows.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_DECLINES_H
#define ENGINE_HOST_FIXTURE_DECLINES_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DECLINES[] =
    "<!doctype html><html><head><base href=\"/api/decl/sub/\"></head>"
    "<body>"
    "<script>"
    "fetch('/api/decl/json', { method: 'POST', body: JSON.stringify({ a: 1 }) });"
    "var dx = new XMLHttpRequest(); dx.open('patch', 'rel-xhr'); dx.send('p');"
    "var dmc = new MessageChannel(); dmc.port1.onmessage = function () {}; dmc.port2.postMessage('behind');"
    "fetch('/api/decl/redir');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
