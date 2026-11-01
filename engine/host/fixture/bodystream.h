/* THE UNKNOWN BODY STREAM DOCUMENT: Fetch §5.3 "Body mixin"'s `body` over a Response whose body is an unknown
   with no example (`new Response(state.*)`, §5.2's string arm). The stream holds one chunk, a Uint8Array, and
   is then closed, so the first read answers done=false and the second done=true. A body whose stream was handed
   out but never read still answers text(); one whose stream is locked to a reader is unusable, so text()
   rejects with a TypeError.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_BODYSTREAM_H
#define ENGINE_HOST_FIXTURE_BODYSTREAM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_BODYSTREAM[] =
    "<!doctype html><html><body>"
    "<script>"
    /* Each unknown is narrowed to a string first, as §5.2's string arm converts it, so text() resolves with a
       string rather than a value promise resolution must test for a `then`. */
    "if (typeof state.bsA === 'string') {"
    "  var bsA = new Response(state.bsA).body.getReader();"
    "  bsA.read().then(function (c1) {"
    "    var k = c1.value instanceof Uint8Array ? 'bsU8' : 'bsOTHER';"
    "    return bsA.read().then(function (c2) {"
    "      fetch('/api/bsread?k=' + k + '&d1=' + c1.done + '&d2=' + c2.done);"
    "    });"
    "  });"
    "}"
    "if (typeof state.bsB === 'string') {"
    "  var bsB = new Response(state.bsB);"
    "  var bsBs = bsB.body;"
    "  bsB.text().then(function (t) {"
    "    fetch('/api/bstext?w=' + typeof t + '&s=' + (bsBs instanceof ReadableStream));"
    "  });"
    "}"
    "if (typeof state.bsC === 'string') {"
    "  var bsC = new Response(state.bsC);"
    "  bsC.body.getReader();"
    "  bsC.text().then(function () { fetch('/api/bslock?w=bsREAD'); },"
    "                  function (e) { fetch('/api/bslock?w=' + e.name); });"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
