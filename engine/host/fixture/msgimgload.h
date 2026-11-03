/* THE MESSAGE-THEN-LOAD DOCUMENT: a `message` listener with no origin check keeps `e.data.v` in a script-level
   binding, and an Image's `load` handler, registered by separate code, writes that binding into markup. A later
   script sets the image's `src` to a 1x1 PNG data: URL. Nothing links the message and the load but the binding,
   so the sink is reached tainted only in a world where the message arrives before the image's load task runs.
   Every witness fetch carries constants only.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MSGIMGLOAD_H
#define ENGINE_HOST_FIXTURE_MSGIMGLOAD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_MSGIMGLOAD[] =
    "<!doctype html><html><body>"
    "<div id='miOut'></div>"
    "<script>"
    "var miStash = '';"
    "window.addEventListener('message', function miMessage(e) {"
    "  if (e.data && typeof e.data.v === 'string') { miStash = e.data.v; fetch('/api/mi-stash'); }"
    "});"
    "</script>"
    "<script>"
    "var miImg = new Image();"
    "miImg.onload = function miOnLoad() {"
    "  fetch('/api/mi-load');"
    "  document.getElementById('miOut').innerHTML = miStash;"
    "};"
    "</script>"
    "<script>"
    "miImg.src = 'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/"
    "iZk9HQAAAABJRU5ErkJggg==';"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
