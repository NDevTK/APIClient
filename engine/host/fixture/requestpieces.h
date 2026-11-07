/* THE REQUEST-PIECES DOCUMENT: requests whose header or body a query value supplies through the two producers
   whose pieces fetch's own header list does not carry. An XMLHttpRequest sets an author header and sends a body
   from `?x=`, and a fetch POSTs a body whose one byte the page writes from `?y=`'s character code. Each carries
   the security-relevant mark naming that query value as the piece's root. A fetch POSTs two body bytes, one from
   `?z=`'s character code and one from the fragment's, so its mark names both sources and its delivery needs
   both.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_REQUESTPIECES_H
#define ENGINE_HOST_FIXTURE_REQUESTPIECES_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_REQUESTPIECES[] =
    "<!doctype html><html><body>"
    "<script>"
    "var rpQ = new URLSearchParams(location.search);"
    "var rpX = rpQ.get('x');"
    "if (rpX !== null) {"
    "  var rpXhr = new XMLHttpRequest();"
    "  rpXhr.open('POST', '/api/rp-xhr');"
    "  rpXhr.setRequestHeader('X-Rp', rpX);"
    "  rpXhr.send(rpX);"
    "}"
    "var rpY = rpQ.get('y');"
    "if (rpY !== null) {"
    "  var rpBytes = new Uint8Array(1);"
    "  rpBytes[0] = rpY.charCodeAt(0);"
    "  fetch('/api/rp-bytes', { method: 'POST', body: rpBytes });"
    "}"
    "var rpZ = rpQ.get('z'), rpH = location.hash.slice(1);"
    "if (rpZ !== null && rpH !== '') {"
    "  var rpTwo = new Uint8Array(2);"
    "  rpTwo[0] = rpZ.charCodeAt(0);"
    "  rpTwo[1] = rpH.charCodeAt(0);"
    "  fetch('/api/rp-two', { method: 'POST', body: rpTwo });"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
