/* THE MESSAGE STATE-MACHINE DOCUMENT: a `message` listener registered only under `?mode=dev`, whose handler
   needs three ordered messages (`init`, `cfg`, `go`), each of a stated shape and from an origin passing a
   forgeable `indexOf` test, before it writes `e.data.html` into markup. Nothing in the page posts a message.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MSGCHAIN_H
#define ENGINE_HOST_FIXTURE_MSGCHAIN_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MSGCHAIN[] =
    "<!doctype html><html>"
    "<body>"
    "<script>"
    /* The emissions are constants, so a reached arm is told apart from an arm that never ran. */
    "var mcEl = document.createElement('div'); document.body.appendChild(mcEl);"
    "if (new URLSearchParams(location.search).get('mode') === 'dev') {"
    " fetch('/api/mcgate?v=mcLISTEN');"
    " var mcState = 0;"
    " window.addEventListener('message', function (e) {"
    "  if (e.origin.indexOf('trusted.example') === -1) return;"
    "  var d = e.data;"
    "  if (!d || typeof d !== 'object') return;"
    "  if (mcState === 0) {"
    "   if (d.type === 'init' && typeof d.session === 'string') { mcState = 1; fetch('/api/mcstep?v=mcINIT'); }"
    "   return;"
    "  }"
    "  if (mcState === 1) {"
    "   if (d.type === 'cfg' && d.version === 2 && typeof d.theme === 'string') {"
    "    mcState = 2; fetch('/api/mcstep?v=mcCFG');"
    "   }"
    "   return;"
    "  }"
    "  if (mcState === 2 && d.type === 'go' && typeof d.html === 'string') {"
    "   fetch('/api/mcstep?v=mcGO'); mcEl.innerHTML = d.html;"
    "  }"
    " });"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
