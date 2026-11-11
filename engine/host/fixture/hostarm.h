/* THE HOST-WORK-ARM DOCUMENT: the example's world paints a large document while a world resting on an assumption
   waits. The first script builds the document, then branches over a localStorage key the area lacks: the arm
   taking it as present is priced (solver/flow.c's FLOW_PRICED_WORK) and throws from a microtask. The example's
   world runs a second program, so it stands at that program's entry, and throws from a timer its animation frame
   callback (HTML §8.1.7.3 update the rendering step 14) queues, so its throw follows the rendering task's paint
   (step 22). The paint is host work with no page frame standing; it serves no code site, so the example's world
   keeps the thread through it and throws first. One of test_forced.c's exploring documents (TF_DOCS); it ends
   with TF_CANVAS_INK, which test_forced.c defines before including it. */
#ifndef ENGINE_HOST_FIXTURE_HOSTARM_H
#define ENGINE_HOST_FIXTURE_HOSTARM_H
#ifndef TF_CANVAS_INK
#error "a fixture document is included before TF_CANVAS_INK, the page ink every document ends with"
#endif

static const char HTML_HOSTARM[] =
    "<!doctype html><html><body>"
    "<script>"
    "document.body.insertAdjacentHTML('beforeend',"
    " '<div><p>haRow <b>one</b> two <i>three</i> four</p><p>five six seven eight</p></div>'.repeat(3000));"
    "var haW = localStorage.getItem('haTok') !== null ? 'haLS' : 'haOBS';"
    "fetch('/api/haarm?w=' + haW);"
    "if (haW === 'haLS') queueMicrotask(function () { throw new Error('haBOOMhaLS'); });"
    "</script>"
    "<script>"
    "if (haW === 'haOBS') requestAnimationFrame(function () {"
    " setTimeout(function () { throw new Error('haBOOMhaOBS'); }, 0); });"
    "</script>"
    TF_CANVAS_INK
    "</body></html>";

#endif
