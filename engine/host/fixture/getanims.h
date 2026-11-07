/* THE GET-ANIMATIONS DOCUMENT (`getanims`): Web Animations' Animatable `getAnimations({ subtree: true })` over an
   animated `<svg>`, excalidraw's own inline call, unguarded. The svg and its `<g>` both run CSS Animations'
   `gaSpin`, so the call answers their two CSSAnimation objects and the page sets each one's `startTime` to 0:
   `n` is 2 and `e` is empty. `t` is the receiver's class string and `m` the message of any error, so a run states
   which receiver the call reached.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GETANIMS_H
#define ENGINE_HOST_FIXTURE_GETANIMS_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GETANIMS[] =
    "<!doctype html><html><head><style>"
    "@keyframes gaSpin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }"
    ".ga { animation: gaSpin 1s linear infinite; }"
    "</style></head><body>"
    "<div><svg class=ga viewBox=\"0 0 10 10\" width=10 height=10><g class=ga><path d=\"M0 0L10 10\"/></g></svg>"
    "<script>"
    "var gaS = document.currentScript.previousElementSibling, gaN = -1, gaE = '', gaM = '';"
    "try {"
    "  var gaL = gaS.getAnimations({ subtree: true });"
    "  gaL.forEach(function (a) { a.startTime = 0; });"
    "  gaN = gaL.length;"
    "} catch (e) { gaE = e.name; gaM = String(e.message); }"
    "fetch('/api/getanims?n=' + gaN + '&e=' + gaE + '&t=' + encodeURIComponent(Object.prototype.toString.call(gaS)) +"
    "      '&m=' + encodeURIComponent(gaM));"
    "</script></div>"
    TF_CANVAS_INK
    "</body></html>";

#endif
