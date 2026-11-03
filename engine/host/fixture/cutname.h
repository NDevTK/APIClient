/* THE LONG-BOOT NAME DOCUMENT: cutboot's setup ahead of the one read of `window.name`, which is written to markup
   and fires. The name (HTML §7.2.2.1 Opening and closing windows) is the navigable's, set by the page that
   navigated it here, and the read is its first, so a candidate can start at a snapshot taken before it instead of
   re-running the setup from the pre-boot baseline.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CUTNAME_H
#define ENGINE_HOST_FIXTURE_CUTNAME_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CUTNAME[] =
    "<!doctype html><html><body>"
    "<div id='cnOut'></div>"
    "<script>"
    "var cnAcc = 0;"
    "for (var cnI = 0; cnI < 4000000; cnI++) cnAcc = (cnAcc + cnI) % 1000003;"
    "document.getElementById('cnOut').innerHTML = window.name;"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
