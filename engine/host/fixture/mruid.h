/* THE RANDOM-UID DOCUMENT: core-js's uid as Next.js's polyfill chunk ships it, Number.prototype.toString
   uncurried (`Function.prototype.call.bind(1..toString)`) and called over `++id + Math.random()`, so the
   receiver of ECMAScript §21.1.3.6 is an unknown Number; the two uids then key one object. The witness fetch
   carries a constant.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MRUID_H
#define ENGINE_HOST_FIXTURE_MRUID_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MRUID[] =
    "<!doctype html><html><body>"
    "<script>"
    "var st = 0, ct = Math.random(), ft = Function.prototype.call.bind(1..toString);"
    "function uid(t) { return 'Symbol(' + t + ')_' + ft(++st + ct, 36); }"
    "var o = {}; o[uid('src')] = 1; o[uid('IE_PROTO')] = 2;"
    "fetch('/api/mru-built?n=' + Object.keys(o).length);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
