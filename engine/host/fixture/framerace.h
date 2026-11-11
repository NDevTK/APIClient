/* The frame race document. One of test_forced.c's exploring documents (its TF_DOCS table). A request is in
   flight while an animation frame callback and an IntersectionObserver are waiting for a rendering opportunity.
   HTML §8.1.7.3 Processing model determines a rendering opportunity from the display and the page, not from the
   network, so whether the frame or the reply's arrival comes first is the race a timer's deadline has: both
   orders run. The reaction and each callback
   report the order they ran in. It ends with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_FRAMERACE_H
#define ENGINE_HOST_FIXTURE_FRAMERACE_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_FRAMERACE[] =
    "<!doctype html><html><body>"
    "<div id='fcBox' style='height: 10px'></div>"
    "<script>"
    "var fcSeen = [], fcReplied = 'fcPending';"
    "function fcDone(t) { fcSeen.push(t); if (fcSeen.length === 2) fetch('/api/fcorder?v=' + fcSeen.join('-')); }"
    "fetch('/api/fcreply').then(function () { fcReplied = 'fcArrived'; fcDone('reply'); });"
    "requestAnimationFrame(function () { fcDone('frame'); });"
    "new IntersectionObserver(function (entries, io) {"
    "  io.disconnect(); fetch('/api/fcobserved?r=' + fcReplied);"
    "}).observe(document.getElementById('fcBox'));"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
