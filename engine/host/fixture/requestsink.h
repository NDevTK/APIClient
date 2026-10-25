/* THE REQUEST-SINK DOCUMENT: requests whose address, header or body an attacker source roots, one source per
   request. Three must carry the security-relevant mark: a credentialed fetch to an address the fragment
   supplies, a same-origin fetch whose path a query value extends, and a POST whose header and body query values
   supply. Five must stay plain API records: an uncredentialed GET to another origin, an uncredentialed GET whose
   origin the page pins, a DELETE the page sends only from its own button's click listener, a fetch in a function
   the page never calls (reached only by a drive), and a credentialed fetch rooted in stored state.
   Exploring documents of test_forced.c (its TF_DOCS table); each ends with TF_CANVAS_INK. */
#ifndef ENGINE_HOST_FIXTURE_REQUESTSINK_H
#define ENGINE_HOST_FIXTURE_REQUESTSINK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REQUESTSINK[] =
    "<!doctype html><html><body>"
    "<button id='rqDel'>Delete</button>"
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
    "function rqNeverCalled() { fetch('/api/rq-orphan/' + rqQ.get('o')); }"
    "var rqStored = localStorage.getItem('rqk');"
    "if (rqStored !== null) fetch('/api/rq-store?k=' + encodeURIComponent(rqStored));"
    "document.getElementById('rqDel').addEventListener('click', function () {"
    "  var d = rqQ.get('d');"
    "  if (d !== null) fetch('/api/drafts/' + d, { method: 'DELETE' });"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
