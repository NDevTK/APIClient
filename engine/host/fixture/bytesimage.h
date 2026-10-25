/* THE BYTES-IMAGE DOCUMENT: a reply read with arrayBuffer() is gated on its first byte through a DataView, which
   forks because the buffer's bytes are an image of the reply's unknown (byte_reader.c). Then the page writes a
   concrete 1 over that byte through a Uint8Array and gates on it again: the write replaced the byte the image
   recorded, so the second gate is decided by the 1 and takes one arm. The first gate is the positive control
   and the second the negative one, over one buffer.
   A second reply's length gates a branch (the length is the reply's unknown too), and a DataView read far past its
   example bytes takes both arms of the in-range test: a value with no example, or the RangeError. An element
   read past them is undefined in the example world and a byte where the reply is longer, and a for-of over the
   bytes runs past the example in those worlds.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_BYTESIMAGE_H
#define ENGINE_HOST_FIXTURE_BYTESIMAGE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_BYTESIMAGE[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var ab = await (await fetch('/api/bimg')).arrayBuffer();"
    "  var dv = new DataView(ab);"
    "  if (dv.getUint8(0) === 123) fetch('/api/bimg/lead-brace'); else fetch('/api/bimg/lead-other');"
    "  new Uint8Array(ab)[0] = 1;"
    "  if (dv.getUint8(0) === 1) fetch('/api/bimg/wrote-one'); else fetch('/api/bimg/wrote-other');"
    "})();"
    "(async function () {"
    "  var ab = await (await fetch('/api/bimg2')).arrayBuffer();"
    "  if (ab.byteLength > 1000) fetch('/api/bimg/len-long'); else fetch('/api/bimg/len-short');"
    "  var dv = new DataView(ab);"
    "  try { dv.getUint32(4000); fetch('/api/bimg/far-read'); } catch (e) {"
    "    fetch(e instanceof RangeError ? '/api/bimg/far-range' : '/api/bimg/far-other'); }"
    "  var by = new Uint8Array(ab);"
    "  if (by[5000] === undefined) fetch('/api/bimg/past-undef'); else fetch('/api/bimg/past-byte');"
    "  var n = 0; for (var b of by) n++;"
    "  if (n > 30) fetch('/api/bimg/iter-long'); else fetch('/api/bimg/iter-short');"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
