/* THE FILE-PARSE (FETCH) DOCUMENT: the page fetches the file its query names (an embedder's `?file=` shape),
   falling back to its own seed file, and walks a tiny binary format of this fixture's own over the bytes: a
   four-byte magic `FPX1`, a version byte, a length byte, and that many bytes of body, decoded with TextDecoder
   and compiled with `new Function`. The seed (TF_SERVED's `/files/fpseed.bin`) is benign. The question is
   whether the file's bytes keep their provenance through the reply, DataView, Uint8Array and the decoder to
   the code-construction sink, with the file, and through it the query, as the sink's source. Each witness
   fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FILEPARSEFETCH_H
#define ENGINE_HOST_FIXTURE_FILEPARSEFETCH_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FILEPARSE_FETCH[] =
    "<!doctype html><html><body>"
    "<script>"
    "function fpfParse(buf) {"
    "  var dv = new DataView(buf);"
    "  if (dv.getUint32(0) !== 0x46505831 || dv.getUint8(4) !== 1) { fetch('/api/fpf-badmagic'); return null; }"
    "  var len = dv.getUint8(5);"
    "  var body = new TextDecoder().decode(new Uint8Array(buf, 6, len));"
    "  fetch('/api/fpf-parsed');"
    "  return new Function('x', body);"
    "}"
    "(async function () {"
    "  var q = location.search, at = q.indexOf('file=');"
    "  var url = at >= 0 ? decodeURIComponent(q.slice(at + 5).split('&')[0]) : '/files/fpseed.bin';"
    "  var buf = await (await fetch(url)).arrayBuffer();"
    "  var fn = fpfParse(buf);"
    "  if (fn) fetch('/api/fpf-built');"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
