/* THE FILE-PARSE (SLICE) DOCUMENT: fileparse-fetch's format and query, with the body taken by copying instead of
   viewing. The page fetches the file its query names (falling back to TF_SERVED's `/files/fpseed.bin`), checks
   the magic and version through a DataView, and decodes the body from `buf.slice(6, 6 + len)` (ECMAScript
   §25.1.6.7 ArrayBuffer.prototype.slice) into `new Function`. A copy carries the bytes' provenance exactly as a
   view does: the seed's sink is rooted in the seed's reply, and the query's in location.search. Each witness
   fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FILEPARSESLICE_H
#define ENGINE_HOST_FIXTURE_FILEPARSESLICE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FILEPARSE_SLICE[] =
    "<!doctype html><html><body>"
    "<script>"
    "function fpsParse(buf) {"
    "  var dv = new DataView(buf);"
    "  if (dv.getUint32(0) !== 0x46505831 || dv.getUint8(4) !== 1) { fetch('/api/fps-badmagic'); return null; }"
    "  var len = dv.getUint8(5);"
    "  var body = new TextDecoder().decode(buf.slice(6, 6 + len));"
    "  fetch('/api/fps-parsed');"
    "  return new Function('x', body);"
    "}"
    "(async function () {"
    "  var q = location.search, at = q.indexOf('file=');"
    "  var url = at >= 0 ? decodeURIComponent(q.slice(at + 5).split('&')[0]) : '/files/fpseed.bin';"
    "  var buf = await (await fetch(url)).arrayBuffer();"
    "  if (fpsParse(buf)) fetch('/api/fps-built');"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
