/* THE FILE-PARSE (SUBARRAY) DOCUMENT: fileparse-slice's question asked of a view instead of a copy. The page
   fetches the file its query names (falling back to TF_SERVED's `/files/fpseed.bin`), checks the magic and
   version through a DataView, and decodes the body from `new Uint8Array(buf).subarray(6, 6 + len)` (ECMAScript
   §23.2.3.30 %TypedArray%.prototype.subarray) into `new Function`. The seed's sink is rooted in the seed's
   reply, and the query's in location.search. Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FILEPARSESUB_H
#define ENGINE_HOST_FIXTURE_FILEPARSESUB_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FILEPARSE_SUB[] =
    "<!doctype html><html><body>"
    "<script>"
    "function fpuParse(buf) {"
    "  var dv = new DataView(buf);"
    "  if (dv.getUint32(0) !== 0x46505831 || dv.getUint8(4) !== 1) { fetch('/api/fpu-badmagic'); return null; }"
    "  var len = dv.getUint8(5);"
    "  var body = new TextDecoder().decode(new Uint8Array(buf).subarray(6, 6 + len));"
    "  fetch('/api/fpu-parsed');"
    "  return new Function('x', body);"
    "}"
    "(async function () {"
    "  var q = location.search, at = q.indexOf('file=');"
    "  var url = at >= 0 ? decodeURIComponent(q.slice(at + 5).split('&')[0]) : '/files/fpseed.bin';"
    "  var buf = await (await fetch(url)).arrayBuffer();"
    "  if (fpuParse(buf)) fetch('/api/fpu-built');"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
