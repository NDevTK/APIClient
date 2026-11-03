/* THE MESSAGE-BEFORE-LOAD DOCUMENT: a `message` listener with no origin check keeps `e.data.v` and `e.data.w` in
   script-level bindings. Separately, a script creates an Image, adds a `load` listener that writes the first
   binding into markup, and sets its `src` to a 1x1 PNG data: URL; and an <img> in the markup, whose `load`
   listener is added at boot, writes the second. Nothing orders the message against either load: the sink is
   reached tainted only in a world where the attacker's message is delivered before the image's load task runs,
   and in a world where the load runs first it writes the binding's initial value. Each load handler's witness
   fetch carries a constant naming which of the two worlds it ran in.
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
    "<div id='miOut'></div><div id='miOut2'></div>"
    "<img id='miMarkup' src='" MSGIMGLOAD_PNG "'>"
    "<script>"
    "var miStash = '', miStash2 = '';"
    "window.addEventListener('message', function miMessage(e) {"
    "  if (!e.data || typeof e.data !== 'object') return;"
    "  if (typeof e.data.v === 'string') { miStash = e.data.v; fetch('/api/mi-stash'); }"
    "  if (typeof e.data.w === 'string') { miStash2 = e.data.w; fetch('/api/mi-stash2'); }"
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
    TF_CANVAS_INK
    "</body></html>";
#undef MSGIMGLOAD_PNG

#endif
