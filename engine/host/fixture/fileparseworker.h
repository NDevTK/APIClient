/* THE FILE-PARSE (WORKER) DOCUMENT: the fileparse-fetch document's file, fetched the same way (the query's
   `file=` or the seed), transferred to a dedicated Worker (TF_SERVED's `/chunk/fpworker.js`) by postMessage;
   the worker walks the FPX1 format and compiles the body with `new Function`. The bytes must keep their
   provenance across the transfer, so the sink in the worker's realm names the file, and through it the query,
   as its source. Each witness fetch carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FILEPARSEWORKER_H
#define ENGINE_HOST_FIXTURE_FILEPARSEWORKER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FILEPARSE_WORKER[] =
    "<!doctype html><html><body>"
    "<script>"
    "(async function () {"
    "  var q = location.search, at = q.indexOf('file=');"
    "  var url = at >= 0 ? decodeURIComponent(q.slice(at + 5).split('&')[0]) : '/files/fpseed.bin';"
    "  var buf = await (await fetch(url)).arrayBuffer();"
    "  var fpwWorker = new Worker('/chunk/fpworker.js');"
    "  fpwWorker.postMessage(buf, [buf]);"
    "  fetch('/api/fpw-posted');"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
