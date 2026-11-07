/* THE HELD-DELIVERY DOCUMENT: cutboot's setup, then one read of the fragment, then a timer whose branch over the
   fragment marks a step done and asks `/api/chready`, on which a cross-origin peer posts once (test_forced.c's
   fixture_route_gadget_post). The `message` listener writes the fragment into markup only once the step is done.
   The peer's post is routed after the fragment's snapshot, so an arm of that snapshot holds it on its queue; the
   path it replays delivered it after the timer, and an arm that delivered it at its first task boundary would run
   the listener before the timer and leave that path with the sink unreached.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CUTHOLD_H
#define ENGINE_HOST_FIXTURE_CUTHOLD_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CUTHOLD[] =
    "<!doctype html><html><body>"
    "<div id='chOut'></div>"
    "<script>"
    "var chAcc = 0, chStep = false;"
    "for (var chI = 0; chI < 4000000; chI++) chAcc = (chAcc + chI) % 1000003;"
    "var chH = decodeURIComponent(location.hash.slice(1));"
    "window.addEventListener('message', function chRecv(e) {"
    "  if (chStep && e.data === 'chgo') { fetch('/api/ch-recv'); document.getElementById('chOut').innerHTML = chH; }"
    "});"
    "setTimeout(function chTick() { if (chH.length > 2) { chStep = true; fetch('/api/chready'); } }, 0);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
