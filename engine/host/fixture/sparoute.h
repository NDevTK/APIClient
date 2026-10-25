/* THE SPA-ROUTE DOCUMENT: a hash router loads its `#/reports` route's chunk on demand by dynamic import, and the
   chunk's mount registers a click handler that reads the query's `q` into innerHTML. The chunk is served by
   this host (TF_SERVED's `/chunk/srreports.js`) and nothing in this document names its handler, so the sink is
   reached only by taking the route, loading and running the chunk, and driving the handler it registered. The
   PoC needs both address components: the route's fragment and the query.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_SPAROUTE_H
#define ENGINE_HOST_FIXTURE_SPAROUTE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_SPAROUTE[] =
    "<!doctype html><html><body>"
    "<div id='srView'></div>"
    "<script>"
    "function srRoute() {"
    "  if (location.hash === '#/reports') {"
    "    fetch('/api/sr-route');"
    "    import('/chunk/srreports.js').then(function (m) { m.srMount(document.getElementById('srView')); });"
    "  }"
    "}"
    "window.addEventListener('hashchange', srRoute);"
    "srRoute();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
