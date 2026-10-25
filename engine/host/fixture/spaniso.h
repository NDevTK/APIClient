/* THE SPAN-ISOLATION DOCUMENT: a typed array created before any fork is the shared baseline, one arm of a fork on
   location.search stores into it at an unknown index (ECMAScript §10.4.5.18 TypedArraySetElement with an index
   only the attacker's input states, which the engine keeps as a span over the buffer rather than a byte), and
   the sibling arm reads the element it never wrote from a later timer task. Per-flow isolation means the
   sibling reads its own 5; a span the writer left in the baseline would answer it an unknown instead.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SPANISO_H
#define ENGINE_HOST_FIXTURE_SPANISO_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SPANISO[] =
    "<!doctype html><html><body>"
    "<script>"
    "var shared = new Uint8Array(8);"
    "shared[3] = 5;"
    "(function () {"
    "  var q = location.search;"
    "  if (q.indexOf('w') >= 0) {"
    "    shared[q.length] = 9;"
    "    fetch('/api/spi-wrote');"
    "  } else {"
    "    setTimeout(function () { fetch('/api/spi-read?v=' + shared[3]); }, 10);"
    "  }"
    "})();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
