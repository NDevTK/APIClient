/* THE FRAME-MESSAGE DOCUMENT: a `message` listener renders `e.data.html` into markup when `e.data.kind` is
   `render`, with no origin check. No window in this fixture posts to it, so the listener is reachable only as a
   cross-document message from a window the attacker holds. The document is rooted with the opener policy
   `same-origin` (its TF_DOCS row), as a page served with `Cross-Origin-Opener-Policy: same-origin` is: a
   cross-origin opener's handle to it is severed, so a PoC must route its message through a frame the attacker
   embeds this page in, never through a window the attacker opened.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRAMEMSG_H
#define ENGINE_HOST_FIXTURE_FRAMEMSG_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FRAMEMSG[] =
    "<!doctype html><html><body>"
    "<div id='fmOut'></div>"
    "<script>"
    "window.addEventListener('message', function fmOnMessage(e) {"
    "  if (e.data && e.data.kind === 'render') {"
    "    fetch('/api/fm-render');"
    "    document.getElementById('fmOut').innerHTML = e.data.html;"
    "  }"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
