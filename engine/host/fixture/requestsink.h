/* THE REQUEST-SINK DOCUMENT: requests whose address, header or body an attacker source roots, one source per
   request. A credentialed fetch to an address the fragment supplies, and one a button's click listener sends to
   an address a query value supplies, are request-class findings (the attacker picks the host). A same-origin
   fetch whose path a query value extends, and a POST whose header and body query values supply, carry the
   security-relevant mark. Five stay plain API records: an uncredentialed GET to another origin, an
   uncredentialed GET whose origin the page pins, a DELETE the page sends only from its own button's click
   listener, a fetch in a function the page never calls (reached only by a drive), and a credentialed fetch
   rooted in stored state.
   Exploring documents of test_forced.c (its TF_DOCS table); each ends with TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_REQUESTSINK_H
#define ENGINE_HOST_FIXTURE_REQUESTSINK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REQUESTSINK[] =
    "<!doctype html><html><body>"
    "<button id='rqDel'>Delete</button><button id='rqGoBtn'>Go</button>"
    "<script>"
    "var rqQ = new URLSearchParams(location.search);"
    "var rqTo = location.hash.slice(1);"
    "if (rqTo !== '') fetch(rqTo, { credentials: 'include' });"
    "var rqId = rqQ.get('id');"
    "if (rqId !== null) fetch('/api/users/' + rqId);"
    "var rqTenant = rqQ.get('t');"
    "if (rqTenant !== null) fetch('/api/rq-forge', { method: 'POST', headers: { 'X-Tenant': rqTenant },"
    "                                                body: rqQ.get('b') });"
    "var rqH = rqQ.get('h');"
    "if (rqH !== null) fetch('https://rq-other.test/q?x=' + encodeURIComponent(rqH), { credentials: 'omit' });"
    "var rqP = rqQ.get('p');"
    "if (rqP !== null) fetch(location.origin + '/api/rq-pinned?x=' + encodeURIComponent(rqP), { credentials: 'omit' });"
    "function rqNeverCalled() { var o = rqQ.get('o'); if (o !== null) fetch('/api/rq-orphan/' + o); }"
    "var rqStored = localStorage.getItem('rqk');"
    "if (rqStored !== null) fetch('/api/rq-store?k=' + encodeURIComponent(rqStored));"
    "document.getElementById('rqGoBtn').addEventListener('click', function () {"
    "  fetch(rqQ.get('go'), { credentials: 'include' });"
    "});"
    "document.getElementById('rqDel').addEventListener('click', function () {"
    "  var d = rqQ.get('d');"
    "  if (d !== null) fetch('/api/drafts/' + d, { method: 'DELETE' });"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
