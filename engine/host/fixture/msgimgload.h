/* THE MESSAGE-BEFORE-LOAD DOCUMENT: a `message` listener with no origin check keeps `e.data.v` and `e.data.w` in
   script-level bindings. Separately, a script creates an Image, adds a `load` listener that writes the first
   binding into markup, and sets its `src` to a 1x1 PNG data: URL; and an <img> in the markup, whose `load`
   listener is added at boot, writes the second. Nothing orders the message against either load: the sink is
   reached tainted only in a world where the attacker's message is delivered before the image's load task runs,
   and in a world where the load runs first it writes the binding's initial value. A third Image exists only
   under `?preview=1` and loads a network address, so its sink needs the query pin, the message and the load.
   Each load handler's witness fetch carries a constant naming which of the two worlds it ran in.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_MSGIMGLOAD_H
#define ENGINE_HOST_FIXTURE_MSGIMGLOAD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

#define MSGIMGLOAD_PNG "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAF" \
                       "AAH/iZk9HQAAAABJRU5ErkJggg=="
static const char HTML_MSGIMGLOAD[] =
    "<!doctype html><html><body>"
    "<div id='miOut'></div><div id='miOut2'></div><div id='miOut3'></div>"
    "<img id='miMarkup' src='" MSGIMGLOAD_PNG "'>"
    "<script>"
    "var miStash = '', miStash2 = '', miStash3 = '';"
    "window.addEventListener('message', function miMessage(e) {"
    "  if (!e.data || typeof e.data !== 'object') return;"
    "  if (typeof e.data.v === 'string') { miStash = e.data.v; fetch('/api/mi-stash'); }"
    "  if (typeof e.data.w === 'string') { miStash2 = e.data.w; fetch('/api/mi-stash2'); }"
    "  if (typeof e.data.x === 'string') { miStash3 = e.data.x; fetch('/api/mi-stash3'); }"
    "});"
    "</script>"
    "<script>"
    "var miImg = new Image();"
    "miImg.addEventListener('load', function miOnLoad() {"
    "  fetch('/api/mi-load?v=' + (miStash === '' ? 'initial' : 'stashed'));"
    "  document.getElementById('miOut').innerHTML = miStash;"
    "});"
    "miImg.src = '" MSGIMGLOAD_PNG "';"
    "document.getElementById('miMarkup').addEventListener('load', function miMarkupLoad() {"
    "  fetch('/api/mi-load2?v=' + (miStash2 === '' ? 'initial' : 'stashed'));"
    "  document.getElementById('miOut2').innerHTML = miStash2;"
    "});"
    "</script>"
    "<script>"
    "if (new URLSearchParams(location.search).get('preview') === '1') {"
    "  fetch('/api/mi-preview');"
    "  var miPrev = new Image();"
    "  miPrev.addEventListener('load', function miPreviewLoad() {"
    "    fetch('/api/mi-load3?v=' + (miStash3 === '' ? 'initial' : 'stashed'));"
    "    document.getElementById('miOut3').innerHTML = miStash3;"
    "  });"
    "  miPrev.src = '/img/p.png';"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";
#undef MSGIMGLOAD_PNG

#endif
