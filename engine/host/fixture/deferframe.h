/* The deferred-script-beside-frames document. One of test_forced.c's exploring documents (its TF_DOCS table). An
   animation frame chain keeps a rendering opportunity ahead of the clock while a root module script's bytes are
   in flight, the shape of a page whose `<script type=module>` waits behind frames (HTML §13.2.7 The end runs it
   once its fetch is answered). The classic script after it runs for several work quanta, so this host pays the
   bytes before the parse ends and the first frame is still ahead: the race is the delivery arm's, over an
   arrival already answered. The module reports how many frames ran before it. The bytes are test_forced.c's
   TF_SERVED row for `/chunk/df-main.js`; the document ends with TF_CANVAS_INK, which test_forced.c defines
   before including this. */
#ifndef ENGINE_HOST_FIXTURE_DEFERFRAME_H
#define ENGINE_HOST_FIXTURE_DEFERFRAME_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_DEFERFRAME[] =
    "<!doctype html><html><body>"
    "<div id='dfBox' style='height: 10px'></div>"
    "<script>"
    "var dfFrames = 0;"
    "function dfTick() { dfFrames++; if (dfFrames < 3) requestAnimationFrame(dfTick); }"
    "requestAnimationFrame(dfTick);"
    "</script>"
    "<script type='module' src='/chunk/df-main.js'></script>"
    "<script>for (var dfBusy = 0; dfBusy < 60000; dfBusy++) {}</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
