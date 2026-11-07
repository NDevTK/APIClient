/* THE JSON-REST DOCUMENT: a JSON reply of 10,001 records and about 300 KiB (test_forced.c's JSONREST_BODY,
   served as application/json), so each parse of it is several work quanta. The page reads it three ways: with
   Fetch §5.3's `json()` over the reply's bytes, as XHR §3.6.9's `responseType = "json"` response, and with
   `json()` over a Response whose body is a page-built ReadableStream holding the same text (Fetch §2.2.4's fully
   read a body). The three lengths and one record of each are reported once all three have settled: a parse that
   rested between quanta answers the value one that did not does. Each reader asks its own address, because
   one address two requests park on is answered once.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_JSONREST_H
#define ENGINE_HOST_FIXTURE_JSONREST_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_JSONREST[] =
    "<!doctype html><html><body>"
    "<script>"
    "var jrF = fetch('/data/jsonrest-f.json').then(function (r) { return r.json(); });"
    "var jrX = new Promise(function (res) {"
    "  var x = new XMLHttpRequest();"
    "  x.open('GET', '/data/jsonrest-x.json');"
    "  x.responseType = 'json';"
    "  x.onload = function () { res(x.response); };"
    "  x.send();"
    "});"
    "var jrS = fetch('/data/jsonrest-s.json').then(function (r) { return r.text(); }).then(function (t) {"
    "  var bytes = new TextEncoder().encode(t);"
    "  return new Response(new ReadableStream({ start: function (c) { c.enqueue(bytes); c.close(); } })).json();"
    "});"
    "Promise.all([jrF, jrX, jrS]).then(function (v) {"
    "  fetch('/api/jsonrest?f=' + v[0].length + '&x=' + v[1].length + '&s=' + v[2].length +"
    "        '&k=' + v[0][1234].i + '-' + v[1][1234].i + '-' + v[2][1234].i);"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
