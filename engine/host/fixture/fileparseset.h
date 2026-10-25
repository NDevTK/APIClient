/* THE FILE-PARSE (SET) DOCUMENT: fileparse-sub's question asked of a copy made by
   %TypedArray%.prototype.set (ECMAScript §23.2.3.26.2 SetTypedArrayFromTypedArray). The page fetches the file
   its query names (falling back to TF_SERVED's `/files/fpseed.bin`), checks the magic and version through a
   DataView, sets the reply's bytes into a 256-byte view and decodes the body from that copy into `new Function`.
   It also reads the copy one past the seed's own length, which only an unknown source length can have written,
   and sets the reply into a 4-byte view, which fits only where the reply is that short. The seed's sink is
   rooted in the seed's reply, and the query's in location.search. Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FILEPARSESET_H
#define ENGINE_HOST_FIXTURE_FILEPARSESET_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FILEPARSE_SET[] =
    "<!doctype html><html><body>"
    "<script>"
    "function fpzParse(buf) {"
    "  var dv = new DataView(buf);"
    "  if (dv.getUint32(0) !== 0x46505831 || dv.getUint8(4) !== 1) { fetch('/api/fpz-badmagic'); return null; }"
    "  var len = dv.getUint8(5), src = new Uint8Array(buf), out = new Uint8Array(256);"
    "  out.set(src);"
    "  if (out[19] === 0x41) fetch('/api/fpz-past');"
    "  try { new Uint8Array(4).set(src); fetch('/api/fpz-fit'); } catch (e) {}"
    "  var body = new TextDecoder().decode(out.subarray(6, 6 + len));"
    "  fetch('/api/fpz-parsed');"
    "  return new Function('x', body);"
    "}"
    "(async function () {"
    "  var q = location.search, at = q.indexOf('file=');"
    "  var url = at >= 0 ? decodeURIComponent(q.slice(at + 5).split('&')[0]) : '/files/fpseed.bin';"
    "  var buf = await (await fetch(url)).arrayBuffer();"
    "  if (fpzParse(buf)) fetch('/api/fpz-built');"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
