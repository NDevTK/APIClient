/* THE DISABLED CLOSE WATCHER DOCUMENT. A parsed non-modal `<dialog open>` beside a dialog shown with
   showModal(). HTML §4.11.4 "The dialog element"'s insertion steps establish the parsed dialog's close watcher,
   whose get enabled state is false (its computed closed-by state is None), and showModal() establishes the
   modal one's into the same group (HTML §6.10.2 "Close watcher infrastructure": one group is allowed with no
   user activation). The first modelled close request closes the modal dialog only; every later one finds a
   group holding just the disabled watcher, closes nothing and falls through to §6.10.1 "Close requests"'
   alternative processing. Each listener fetches only once its dialog is closed, so a drive of either alone
   sends nothing. Chrome 153 (Playwright, Escape pressed three times) sends `/api/cwmix?a=true` and nothing for
   the open dialog.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_CWMIX_H
#define ENGINE_HOST_FIXTURE_CWMIX_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_CWMIX[] =
    "<!doctype html><html><body><dialog id=a open>a</dialog><dialog id=b>b</dialog>"
    "<script>"
    "var a = document.getElementById('a'), b = document.getElementById('b');"
    "a.addEventListener('close', function cwOnCloseA() { if (!a.open) fetch('/api/cwmix-a'); });"
    "b.addEventListener('close', function cwOnCloseB() { if (!b.open) fetch('/api/cwmix?a=' + a.open); });"
    "b.showModal();"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
