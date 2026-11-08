/* THE RENDERED GET-ANIMATIONS DOCUMENT (`garender`): a CSS animation exists before any script asks for it. Update
   the rendering step 16.2.1 resolves the `<p>`'s style in the first frame, which generates its CSSAnimation and
   plays it; the second frame's step 11 runs its pending play task. So when the second frame's animation frame
   callback asks `document.getAnimations()`, the one animation it answers is not pending and has a start time.
   One of test_forced.c's exploring documents (its TF_DOCS table). It ends with TF_CANVAS_INK, which
   test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_GARENDER_H
#define ENGINE_HOST_FIXTURE_GARENDER_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_GARENDER[] =
    "<!doctype html><html><head><style>"
    "@keyframes grFade { from { opacity: 0; } to { opacity: 1; } }"
    ".gr { animation: grFade 1s linear infinite; }"
    "</style></head><body><p class=gr>x</p>"
    "<script>"
    "requestAnimationFrame(function () { requestAnimationFrame(function () {"
    "  var grL = document.getAnimations(), grA = grL[0];"
    "  fetch('/api/garender?n=' + grL.length + '&p=' + (grA ? grA.pending : 'none') +"
    "        '&s=' + (grA ? grA.startTime !== null : 'none'));"
    "}); });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
