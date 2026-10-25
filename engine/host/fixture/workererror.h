/* THE WORKER-ERROR DOCUMENTS: HTML §8.1.4.6 report an exception for a dedicated worker's uncaught throw. The
   worker's script (TF_SERVED's `/chunk/weerr.js` in its message handler, `/chunk/wcerr.js` at its top level)
   throws, nothing in the worker handles it, and step 7.2 queues a task at the owner that fires `error` at the
   Worker with `error` null.
   `workererror` leaves that event uncancelled, so step 7.2.3 reports it again at the owner's window with
   omitError true; `workererror-cancel` cancels it, so the owner's window sees nothing and no console line is
   written. Each witness fetch carries booleans the engine computes, never a page-chosen value.
   Two of test_forced.c's exploring documents (its TF_DOCS table). Each ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_WORKERERROR_H
#define ENGINE_HOST_FIXTURE_WORKERERROR_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_WORKERERROR[] =
    "<!doctype html><html><body>"
    "<script>"
    "var weWorker = new Worker('/chunk/weerr.js');"
    "weWorker.onerror = function weOnError(e) {"
    "  fetch('/api/we-onerror?ee=' + (e instanceof ErrorEvent) + '&msg=' + (e.message.indexOf('weTHROWN') >= 0)"
    "        + '&file=' + (e.filename.indexOf('/chunk/weerr.js') >= 0) + '&line=' + (e.lineno > 0)"
    "        + '&err=' + (e.error === null) + '&can=' + e.cancelable);"
    "};"
    "window.addEventListener('error', function weOnWindowError(e) {"
    "  fetch('/api/we-window?msg=' + (e.message.indexOf('weTHROWN') >= 0) + '&err=' + (e.error === null));"
    "});"
    "weWorker.postMessage('go');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

static const char HTML_WORKERERROR_CANCEL[] =
    "<!doctype html><html><body>"
    "<script>"
    "var wcWorker = new Worker('/chunk/wcerr.js');"
    "wcWorker.onerror = function wcOnError(e) {"
    "  e.preventDefault();"
    "  fetch('/api/wc-onerror?err=' + (e.error === null) + '&canceled=' + e.defaultPrevented);"
    "};"
    "window.addEventListener('error', function wcOnWindowError(e) {"
    "  fetch('/api/wc-window?msg=' + (e.message.indexOf('wcTHROWN') >= 0));"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
