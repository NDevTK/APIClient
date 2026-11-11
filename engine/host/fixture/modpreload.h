/* The module-preload document: four `<link rel=modulepreload>` in the head name chunks that the document's root
   module script then reaches. `mp-main.js` imports `mp-a.js`, `mp-b.js` and `mp-c.js` statically, `mp-a.js`
   imports `mp-c.js` too, and `mp-d.js` is reached only by a dynamic `import()` queued from the module's top level.
   HTML §8.1.4.2 "Fetching scripts"' fetch a single module script keys its module map by (url, module type), so
   each chunk is fetched once, by its preload, and every import of it takes that fetch's result. The bodies are
   test_forced.c's TF_SERVED rows; the document ends with TF_CANVAS_INK, which test_forced.c defines before
   including this. */
#ifndef ENGINE_HOST_FIXTURE_MODPRELOAD_H
#define ENGINE_HOST_FIXTURE_MODPRELOAD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define MP_PRELOAD(x) \
    "<link rel=modulepreload href='/chunk/mp-" x ".js' onload=\"fetch('/api/mpload?w=mpL" x "')\">"

static const char HTML_MODPRELOAD[] =
    "<!doctype html><html><head>"
    MP_PRELOAD("a") MP_PRELOAD("b") MP_PRELOAD("c") MP_PRELOAD("d")
    "</head><body>"
    "<script type='module' src='/chunk/mp-main.js'></script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
