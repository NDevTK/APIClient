/* THE HELD-DELIVERY DOCUMENT: cutboot's setup, one read of the fragment, then a timer whose branch over the
   fragment marks a step done and asks `/api/chready`, on which a cross-origin peer posts once (test_forced.c's
   fixture_route_gadget_post). The `message` listener records whether the step was done when the post arrived,
   with no branch of its own, and a polling timer writes the fragment into markup once it was.
   The peer's post is routed after the fragment's snapshot, so an arm of that snapshot holds it on its queue. The
   path it replays delivered it after the timer's branch; an arm that delivered it at its first task boundary would
   run the listener before that timer and leave the path with the sink unreached. The poll ends, so the page goes
   quiet and a drive of the listener finds no poll left to reach the sink.
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
    "var chAcc = 0, chStep = false, chGot = false, chTries = 0;"
    "for (var chI = 0; chI < 4000000; chI++) chAcc = (chAcc + chI) % 1000003;"
    "var chH = decodeURIComponent(location.hash.slice(1));"
    "window.addEventListener('message', function chRecv() { chGot = chStep; });"
    "function chPoll() {"
    "  if (chGot) { fetch('/api/ch-recv'); document.getElementById('chOut').innerHTML = chH; }"
    "  else if (chTries++ < 400) setTimeout(chPoll, 0);"
    "}"
    "setTimeout(function chTick() { if (chH.length > 2) { chStep = true; fetch('/api/chready'); chPoll(); } }, 0);"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
