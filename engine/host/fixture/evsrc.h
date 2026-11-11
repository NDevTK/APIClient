/* THE EVENTSOURCE DOCUMENT: HTML §9.2.2 "The EventSource interface" and HTML §9.2.3 "Processing model".
   The first shape is a recording snippet's: `class X extends EventSource` evaluated at boot, which throws a
   ReferenceError where the interface is absent and takes the whole snippet down with it. The second is a stream
   over a `data:` URL, which Fetch §4.3 Scheme fetch answers in the agent: the connection is announced (`open`
   at OPEN), its one event is dispatched (`message` with the stream's data and the final URL's opaque origin),
   and the body's end reestablishes the connection (`error` at CONNECTING), whose handler closes it.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_EVSRC_H
#define ENGINE_HOST_FIXTURE_EVSRC_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_EVSRC[] =
    "<!doctype html><html><body>"
    "<script>"
    "var esC = 'esNONE';"
    "try {"
    "  class EsRec extends EventSource {}"
    "  esC = (typeof EventSource === 'function' && EventSource.CLOSED === 2 &&"
    "         EsRec.prototype instanceof EventSource) ? 'esCLASS' : 'esSHAPE';"
    "} catch (e) { esC = e instanceof ReferenceError ? 'esREF' : 'esTHREW'; }"
    "fetch('/api/es-class?w=' + esC);"
    "var es = new EventSource('data:text/event-stream,data:%20esHI%0A%0A');"
    "fetch('/api/es-ctor?w=' + es.readyState + '&u=' + es.url.slice(0, 5));"
    "es.onopen = function () { fetch('/api/es-open?w=' + es.readyState); };"
    "es.onmessage = function (e) { fetch('/api/es-msg?w=' + e.data + '&o=' + e.origin); };"
    "es.onerror = function () { fetch('/api/es-err?w=' + es.readyState); es.close(); };"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
