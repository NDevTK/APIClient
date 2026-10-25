/* THE WINDOW-NAME DOCUMENT: `window.name` is read on load into a script and into markup. HTML §7.2.2.1 lets
   whoever navigated this navigable name it, so a page an attacker controls sets the name and then navigates
   the same navigable here, and the name survives the navigation. The PoC must state that act: the value is in
   no part of this document's address.
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
