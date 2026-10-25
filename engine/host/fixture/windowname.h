/* THE WINDOW-NAME DOCUMENT: `window.name` is read on load into a script and into markup. The name (HTML
   §7.2.2.1 Opening and closing windows) belongs to the navigable, not the document, so another page sets it:
   an embedder through its iframe's `name` attribute, or an opener that names a window and navigates it here.
   The PoC must state that act, because the value is in no part of this document's address.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_WINDOWNAME_H
#define ENGINE_HOST_FIXTURE_WINDOWNAME_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_WINDOWNAME[] =
    "<!doctype html><html><body>"
    "<div id='wnOut'></div>"
    "<script>"
    "var wnName = window.name;"
    "if (wnName) {"
    "  fetch('/api/wn-named');"
    "  eval(\"var wnTheme = '\" + wnName + \"';\");"
    "  document.getElementById('wnOut').innerHTML = '<p>' + wnName + '</p>';"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
