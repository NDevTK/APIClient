/* THE GETITERATOR-BODY FORK DOCUMENT: an object whose `[Symbol.iterator]` is an ordinary function that branches
   on the fragment, consumed by Array.from. ECMA-262 §7.4.3 GetIteratorFromMethod ( obj, method ) step 1 calls
   that method, so the branch forks while the consumer waits on the call, and each arm returns its own
   one-element iterator.
   Both arms' witness fetches carry the constant their iterator yields.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GETITERFORK_H
#define ENGINE_HOST_FIXTURE_GETITERFORK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GETITERFORK[] =
    "<!doctype html><html><body>"
    "<script>"
    "var gifIterable = {};"
    "gifIterable[Symbol.iterator] = function gifIter() {"
    "  if (location.hash === '#gif') { return ['gifA'][Symbol.iterator](); }"
    "  return ['gifP'][Symbol.iterator]();"
    "};"
    "fetch('/api/gif?v=' + Array.from(gifIterable)[0]);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
