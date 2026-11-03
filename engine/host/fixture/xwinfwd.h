/* THE CROSS-WINDOW FORWARD DOCUMENT: the top-level document A's `message` listener, with no origin check,
   forwards a String's data to its same-origin srcdoc frame B as `{xwFwd: data}`; B's listener accepts only
   messages whose origin is A's and writes `xwFwd` into markup. The attacker reaches B only through A, so B's
   sink is a chain: the attacker's message to A, A's postMessage to B, B's write. Nothing in the page posts to
   A. The frame is markup, so the parse creates it once before anything forks. Every witness fetch carries
   constants only.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_XWINFWD_H
#define ENGINE_HOST_FIXTURE_XWINFWD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_XWINFWD[] =
    "<!doctype html><html><body>"
    "<iframe id=xwB srcdoc=\"<div id=xwOut></div>"
    "<script>"
    "addEventListener('message', function xwBRecv(e) {"
    "  if (e.origin !== 'https://x.test') return;"
    "  if (!e.data || typeof e.data.xwFwd !== 'string') return;"
    "  fetch('https://x.test/api/xw-b-recv');"
    "  document.getElementById('xwOut').innerHTML = e.data.xwFwd;"
    "});"
    "fetch('https://x.test/api/xw-b-ready');"
    "</script>\"></iframe>"
    "<script>"
    "window.addEventListener('message', function xwARecv(e) {"
    "  if (typeof e.data !== 'string') return;"
    "  fetch('/api/xw-a-recv');"
    "  document.getElementById('xwB').contentWindow.postMessage({ xwFwd: e.data }, '*');"
    "});"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
