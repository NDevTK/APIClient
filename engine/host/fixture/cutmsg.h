/* THE LONG-BOOT MESSAGE DOCUMENT: cutboot's setup ahead of a `message` listener whose `data` is written to
   markup. A cross-origin peer posts once when the page asks `/api/cmready` (test_forced.c's
   fixture_route_gadget_post), so the data's first read is the mint in that delivery's task (HTML §9.3.3 Posting
   messages), and a candidate can start at a snapshot taken there instead of re-running the setup from the
   pre-boot baseline, where no second post arrives.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CUTMSG_H
#define ENGINE_HOST_FIXTURE_CUTMSG_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CUTMSG[] =
    "<!doctype html><html><body>"
    "<div id='cmOut'></div>"
    "<script>"
    "var cmAcc = 0;"
    "for (var cmI = 0; cmI < 4000000; cmI++) cmAcc = (cmAcc + cmI) % 1000003;"
    "window.addEventListener('message', function (e) { document.getElementById('cmOut').innerHTML = e.data; });"
    "fetch('/api/cmready');"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
