/* THE FILE-PARSE (TYPED-ARRAY SLICE) DOCUMENT: fileparse-sub's format and query, with the body copied out of a view
   (ECMAScript §23.2.3.27 %TypedArray%.prototype.slice) instead of viewed. The page fetches the file its query
   names (falling back to TF_SERVED's `/files/fpseed.bin`), checks the magic and version through a DataView, and
   decodes `new Uint8Array(buf).slice(6, 6 + len)` into `new Function`, so on the query's arm the end is a byte of
   a reply whose length is unknown. It also slices a page-built array from an index the fragment supplies and
   witnesses the arm whose copy is three elements long. Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FILEPARSETASLICE_H
#define ENGINE_HOST_FIXTURE_FILEPARSETASLICE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FILEPARSE_TASLICE[] =
    "<!doctype html><html><body>"
    "<script>"
    "var fptTail = new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]).slice(+location.hash.slice(1) || 0);"
    "if (fptTail.length === 3) fetch('/api/fpt-hash3');"
    "function fptParse(buf) {"
    "  var dv = new DataView(buf);"
    "  if (dv.getUint32(0) !== 0x46505831 || dv.getUint8(4) !== 1) { fetch('/api/fpt-badmagic'); return null; }"
    "  var len = dv.getUint8(5);"
    "  var body = new TextDecoder().decode(new Uint8Array(buf).slice(6, 6 + len));"
    "  fetch('/api/fpt-parsed');"
    "  return new Function('x', body);"
    "}"
    "(async function () {"
    "  var q = location.search, at = q.indexOf('file=');"
    "  var url = at >= 0 ? decodeURIComponent(q.slice(at + 5).split('&')[0]) : '/files/fpseed.bin';"
    "  var buf = await (await fetch(url)).arrayBuffer();"
    "  if (fptParse(buf)) fetch('/api/fpt-built');"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
