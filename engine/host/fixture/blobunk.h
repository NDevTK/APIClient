/* THE BLOB-UNKNOWN DOCUMENT: Fetch §5.3 Body mixin's blob() over a reply of unknown bytes answers a Blob whose
   byte sequence is that unknown. The host declines the POST (fixture_provide's /api/decl rule), and the decline's
   arm that takes a reply no server sent holds a body with no bytes. On it the Blob's text() is the reply's text,
   so a gate on it forks (bu-text), and its size is the byte sequence's length, so a gate on it forks (bu-size).
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_BLOBUNK_H
#define ENGINE_HOST_FIXTURE_BLOBUNK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_BLOBUNK[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var b = await (await fetch('/api/decl/bu', { method: 'POST' })).blob();"
    "  fetch('/api/bu-type?blob=' + (b instanceof Blob));"
    "  if (b.size > 100) fetch('/api/bu-size/long'); else fetch('/api/bu-size/short');"
    "  if ((await b.text()) === 'ok') fetch('/api/bu-text/ok'); else fetch('/api/bu-text/other');"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
