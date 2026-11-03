/* THE EXAMPLE-FREE WINDOW NAME DOCUMENT: HTML §7.2.2.1's `name` setter handed a DOMString derived from a capture
   of a match over document.referrer, an attacker source. The match arm is forced, so the capture, and the name
   composed around it, hold no example. The name reads back as that unknown, and a branch over it runs both arms.
   The shape is Matomo's Overlay session marker (`window.name = "Matomo_Overlay###" + idSite + …`).
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_WNAMEUNK_H
#define ENGINE_HOST_FIXTURE_WNAMEUNK_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_WNAMEUNK[] =
    "<!doctype html><html><body>"
    "<script>"
    "var wnM = /idSite=([0-9]+)&period=([^&]+)/.exec(document.referrer);"
    "if (wnM) {"
    "  window.name = 'wnP###' + wnM[1] + '###' + wnM[2];"
    "  var wnParts = window.name.split('###');"
    "  fetch('/api/wnunk?w=' + (wnParts[2] === 'day' ? 'wnDAY' : 'wnOTHER'));"
    "}"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
