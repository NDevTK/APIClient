/* THE XHR-TEXT DOCUMENT: XMLHttpRequest's text reply getters (XHR §3.6.10 The responseText getter, and §3.6.9
   The response getter for responseType "" and "text") return the decode of the received bytes, a String whatever
   the server sends, so jQuery's `"string" != typeof r.responseText` takes one arm (no xt-a1, xt-a2, xt-a4). The
   reply is still unknown input: JSON.parse over it keeps both its success and its throw (xt-a3/xt-b3). The
   fixture host answers an address holding `/api/xa` with the body "x". One of test_forced.c's exploring
   documents (TF_DOCS); it ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_XHRTEXT_H
#define ENGINE_HOST_FIXTURE_XHRTEXT_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_XHRTEXT[] =
    "<!doctype html><html><body>"
    "<script>"
    "var xtReq = new XMLHttpRequest(); xtReq.open('GET', '/api/xatext');"
    "xtReq.onload = function () {"
    " var r = xtReq.responseText;"
    " if ('string' != typeof r) fetch('/api/xt-a1'); else fetch('/api/xt-b1');"
    " if ('string' != typeof xtReq.response) fetch('/api/xt-a2'); else fetch('/api/xt-b2');"
    " try { JSON.parse(r); fetch('/api/xt-a3'); } catch (e) { fetch('/api/xt-b3'); }"
    "};"
    "xtReq.send();"
    "var xtTxt = new XMLHttpRequest(); xtTxt.open('GET', '/api/xatext2'); xtTxt.responseType = 'text';"
    "xtTxt.onload = function () {"
    " if (typeof xtTxt.response !== 'string') fetch('/api/xt-a4'); else fetch('/api/xt-b4');"
    "};"
    "xtTxt.send();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
